/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// STEALTH-HOOK(network/proxy-udp)
// STEALTH-HOOK(fingerprint/webrtc/declared-network)
//
// The sockets of a PeerConnection are born in NrSocket::create, and two rules are made there:
//
//   * the proxy: the UDP socket of a PeerConnection gets the layer of its proxy (StealthUdpProxyAttachRefused, the line after
//     PR_OpenUDPSocket). Without that line the socket of a PeerConnection behind a proxy that carries no UDP is a plain one, and its
//     datagrams go out direct, around the proxy: an IP leak. The test makes a real NrSocket under a real config (the lookup of the
//     proxy made as the PeerConnection makes it) and looks at where its datagram goes, with the same socket under "no proxy" as the
//     control (a laboratory in which a direct datagram does not arrive proves nothing).
//   * the port: a port that another socket holds is R_ALREADY (the caller takes the next one, as the kernel would), never R_INTERNAL
//     (REVIEW-63 #12). Where the system answers "access denied" for a privileged port (a user that is not root on Linux; Windows
//     answers it for the ranges it reserves) that answer is the same, and it is asserted when the machine gives it.
//
// Pure behaviour: the NrSocket is the real one, on the socket thread, and the receiver is a plain NSPR socket.

#include "gtest/gtest.h"

#include <memory>
#include <string>
#include <utility>

// nICEr is C++: its headers are not wrapped in extern "C".
// clang-format off
#include <csi_platform.h>
#include "nr_api.h"
#include "transport_addr.h"
// clang-format on

#include "StealthTestProxyKit.h"
#include "nr_stealth_network.h"
#include "nsSOCKSUDPIOLayer.h"
#include "mozilla/RefPtr.h"
#include "mozilla/SyncRunnable.h"
#include "nr_socket_prsock.h"
#include "nr_socket_proxy_config.h"
#include "nsCOMPtr.h"
#include "nsIEventTarget.h"
#include "nsISocketTransportService.h"
#include "nsNetCID.h"
#include "nsServiceManagerUtils.h"
#include "nsThreadUtils.h"
#include "prerror.h"
#include "prio.h"
#include "prnetdb.h"

namespace mozilla {
namespace {

// Runs aFn on the socket transport thread and returns when it has run.
template <typename F>
void OnSocketThread(F&& aFn) {
  nsresult rv;
  nsCOMPtr<nsISocketTransportService> sts = do_GetService(NS_SOCKETTRANSPORTSERVICE_CONTRACTID, &rv);
  ASSERT_TRUE(NS_SUCCEEDED(rv) && sts);
  nsCOMPtr<nsIEventTarget> target = do_QueryInterface(sts);
  ASSERT_TRUE(target);
  ASSERT_EQ(NS_OK, SyncRunnable::DispatchToThread(target, NS_NewRunnableFunction("StealthNrSocketBirth", std::forward<F>(aFn))));
}

nr_transport_addr UdpAddr(const char* aIp, uint16_t aPort) {
  nr_transport_addr a;
  EXPECT_EQ(0, nr_str_port_to_transport_addr(aIp, nullptr, aPort, IPPROTO_UDP, &a));
  return a;
}

// A plain NSPR UDP socket bound to the wildcard, on a port the system chose.
PRFileDesc* PlainSocketOnAnyPort() {
  PRFileDesc* s = PR_NewUDPSocket();
  PRNetAddr any;
  PR_InitializeNetAddr(PR_IpAddrAny, 0, &any);
  EXPECT_EQ(PR_SUCCESS, PR_Bind(s, &any));
  return s;
}

uint16_t PortOf(PRFileDesc* aSocket) {
  PRNetAddr bound;
  EXPECT_EQ(PR_SUCCESS, PR_GetSockName(aSocket, &bound));
  return PR_ntohs(bound.inet.port);
}

// The address of this host that is not loopback (a datagram to the loopback goes direct in every state): where a receiver can stand.
std::string HostAddressString() {
  PRNetAddr probe;
  PR_InitializeNetAddr(PR_IpAddrNull, 9, &probe);
  probe.inet.ip = PR_htonl(0xC0000201);  // 192.0.2.1: nothing is sent
  PRFileDesc* s = PR_NewUDPSocket();
  PRNetAddr mine;
  PR_InitializeNetAddr(PR_IpAddrAny, 0, &mine);
  if (PR_Connect(s, &probe, PR_INTERVAL_NO_WAIT) == PR_SUCCESS) {
    PR_GetSockName(s, &mine);
  }
  PR_Close(s);
  char text[64] = {0};
  EXPECT_EQ(PR_SUCCESS, PR_NetAddrToString(&mine, text, sizeof(text)));
  return text;
}

// A port the system refuses a plain bind to with 'access denied' (0 if it refuses none): the privileged ports where the user is not root, then
// the end of the dynamic range, where Windows keeps its reserved ranges.
uint16_t PortThePlainBindOfThisUserIsDenied() {
  PRFileDesc* probe = PR_NewUDPSocket();
  uint16_t found = 0;
  for (uint32_t port = 1; port < 65536 && found == 0; ++port) {
    if (port == 1024) {
      port = 49152;
    }
    PRNetAddr addr;
    PR_InitializeNetAddr(PR_IpAddrAny, static_cast<uint16_t>(port), &addr);
    if (PR_Bind(probe, &addr) == PR_SUCCESS) {
      PR_Close(probe);  // a socket that is bound cannot try another port
      probe = PR_NewUDPSocket();
    } else if (PR_GetError() == PR_NO_ACCESS_RIGHTS_ERROR) {
      found = static_cast<uint16_t>(port);
    }
  }
  PR_Close(probe);
  return found;
}

// Whether a datagram sent by a real NrSocket made under aConfig reaches a receiver at the host's own (non-loopback) address.
bool DatagramOfANrSocketArrives(const std::shared_ptr<NrSocketProxyConfig>& aConfig) {
  const std::string host = HostAddressString();
  EXPECT_NE(std::string("0.0.0.0"), host) << "this host has no address to a route: a receiver cannot stand";
  // The engine drops, and says it sent, a datagram to a port Necko bans (NrSocketBase::IsForbiddenAddress): a receiver on one would make
  // the Blocked test green for a wrong reason. A port the engine would drop is not taken.
  PRFileDesc* receiver = PlainSocketOnAnyPort();
  nr_transport_addr to = UdpAddr(host.c_str(), PortOf(receiver));
  for (int attempt = 0; attempt < 20 && NrSocketBase::IsForbiddenAddress(&to); ++attempt) {
    PR_Close(receiver);
    receiver = PlainSocketOnAnyPort();
    to = UdpAddr(host.c_str(), PortOf(receiver));
  }
  EXPECT_FALSE(NrSocketBase::IsForbiddenAddress(&to)) << "no receiver port the engine would not drop was found";
  nr_transport_addr local = UdpAddr("0.0.0.0", 0);

  RefPtr<NrSocket> nrSocket = new NrSocket(aConfig);
  int created = -1;
  OnSocketThread([&] {
    created = nrSocket->create(&local);
    if (created == 0) {
      (void)nrSocket->sendto("ping", 4, 0, &to);
    }
  });
  EXPECT_EQ(0, created) << "the socket of the PeerConnection could not be born";

  bool arrived = false;
  if (created == 0) {
    char buf[16];
    PRNetAddr from;
    arrived = PR_RecvFrom(receiver, buf, sizeof(buf), 0, &from, PR_MillisecondsToInterval(500)) > 0;
  }
  OnSocketThread([&] { nrSocket->close(); });
  PR_Close(receiver);
  return arrived;
}

}  // namespace

TEST(StealthNrSocketBirth, TheControlASocketOfAPeerConnectionWithNoProxyIsHeardDirect)
{
  stealthtest::SessionProxyScope scope(stealthtest::ProxyOfTheSession::None);
  ASSERT_TRUE(scope.Ok());
  std::shared_ptr<NrSocketProxyConfig> config = stealthtest::MakeConfigAfterTheLookup();
  ASSERT_TRUE(config);
  EXPECT_TRUE(DatagramOfANrSocketArrives(config))
      << "the laboratory does not see a direct datagram: the test below would prove nothing";
}

TEST(StealthNrSocketBirth, ASocketOfAPeerConnectionBehindAProxyThatCarriesNoUdpSendsNothingDirect)
{
  stealthtest::SessionProxyScope scope(stealthtest::ProxyOfTheSession::HttpWithoutUdp);
  ASSERT_TRUE(scope.Ok());
  std::shared_ptr<NrSocketProxyConfig> config = stealthtest::MakeConfigAfterTheLookup();
  ASSERT_TRUE(config);
  EXPECT_FALSE(DatagramOfANrSocketArrives(config))
      << "a datagram of a PeerConnection behind a proxy without UDP went out direct: NrSocket::create does not put the layer on its "
         "socket";
}

TEST(StealthNrSocketBirth, APortAnotherSocketHoldsIsAlreadyAndTheNextOneIsBorn)
{
  PRFileDesc* holder = PlainSocketOnAnyPort();
  const uint16_t busy = PortOf(holder);
  PRFileDesc* spare = PlainSocketOnAnyPort();
  const uint16_t freePort = PortOf(spare);
  PR_Close(spare);  // a port the system just gave and nobody holds

  nr_transport_addr busyAddr = UdpAddr("0.0.0.0", busy);
  nr_transport_addr freeAddr = UdpAddr("0.0.0.0", freePort);
  RefPtr<NrSocket> refused = new NrSocket();
  RefPtr<NrSocket> born = new NrSocket();
  int busyResult = -1;
  int freeResult = -1;
  OnSocketThread([&] {
    busyResult = refused->create(&busyAddr);
    freeResult = born->create(&freeAddr);
  });
  EXPECT_EQ(R_ALREADY, busyResult) << "a port another socket holds is R_ALREADY (the caller takes the next one), not a failure of the gather";
  EXPECT_EQ(0, freeResult) << "the next port is taken as the kernel would";
  OnSocketThread([&] { born->close(); });  // the refused one never was born: it is only released, close() is for a socket that was
  PR_Close(holder);
}

TEST(StealthNrSocketBirth, APortTheSystemKeepsFromThisUserIsAlreadyToo)
{
  // 'Access denied' on a bind: a privileged port where the user is not root (Linux), a range the system reserves (Windows keeps some of the
  // dynamic range for Hyper-V, WSL and Docker). The machine is asked, port by port, for one it refuses; where it refuses none (a root user,
  // a Windows with no reserved range) there is no such answer to map and nothing is asserted.
  const uint16_t kept = PortThePlainBindOfThisUserIsDenied();
  if (kept == 0) {
    GTEST_LOG_(INFO) << "no port is denied to this user on this machine (root, or no reserved range): nothing to map, nothing asserted";
    return;
  }
  nr_transport_addr addr = UdpAddr("0.0.0.0", kept);
  RefPtr<NrSocket> nrSocket = new NrSocket();
  int result = -1;
  OnSocketThread([&] { result = nrSocket->create(&addr); });
  EXPECT_EQ(R_ALREADY, result) << "'access denied' for port " << kept
                               << " is R_ALREADY (the caller takes the next one), not R_INTERNAL";
  // A socket whose create failed is only released (its close() is for a socket that was born: it asks for the socket thread it remembered).
  if (result == 0) {
    OnSocketThread([&] { nrSocket->close(); });
  }
}

// The socket of a PeerConnection whose layer cannot be pushed must not exist: a plain one would send direct, around the proxy.
TEST(StealthNrSocketBirth, ASocketWhoseLayerCannotBePushedIsNotBorn)
{
  stealthtest::SessionProxyScope scope(stealthtest::ProxyOfTheSession::None);
  ASSERT_TRUE(scope.Ok());
  std::shared_ptr<NrSocketProxyConfig> config = stealthtest::MakeConfigAfterTheLookup();
  ASSERT_TRUE(config);
  nr_transport_addr local = UdpAddr("0.0.0.0", 0);
  RefPtr<NrSocket> refused = new NrSocket(config);
  int result = 0;
  net::nsSOCKSUDPIOLayerFailNextPushForTests(true);
  OnSocketThread([&] { result = refused->create(&local); });
  net::nsSOCKSUDPIOLayerFailNextPushForTests(false);
  EXPECT_EQ(R_INTERNAL, result) << "the socket was born plain although its layer was refused: its datagrams would go out direct";
  // The refusal is for one socket: the next one is born, with its layer.
  RefPtr<NrSocket> next = new NrSocket(config);
  int nextResult = -1;
  OnSocketThread([&] { nextResult = next->create(&local); });
  EXPECT_EQ(0, nextResult);
  OnSocketThread([&] { next->close(); });  // the refused one never was born: it is only released
}

// REVIEW-63 #23: a declared host socket is registered as one of ours (R3) as the LAST step of create, after the socket transport service
// took it. A create that fails at that step must leave no port of ours behind: else a datagram of this browser to that declared port
// would be sent to the loopback (where a socket of someone else may be) instead of to the declared address.
TEST(StealthNrSocketBirth, ACreateThatFailsAtTheLastStepLeavesNoPortOfOursBehind)
{
  const char* kDeclared = "198.51.100.77";  // documentation address: the host does not own it, as a declared one
  // Four ports the system gives together (the sockets are closed together, so no two are the same) and that the engine does not ban.
  PRFileDesc* probes[4];
  uint16_t ports[4];
  for (int i = 0; i < 4; ++i) {
    probes[i] = PlainSocketOnAnyPort();
    ports[i] = PortOf(probes[i]);
    nr_transport_addr checked = UdpAddr(kDeclared, ports[i]);
    EXPECT_FALSE(NrSocketBase::IsForbiddenAddress(&checked)) << "port " << ports[i] << " is one the engine drops datagrams to";
  }
  for (PRFileDesc* probe : probes) {
    PR_Close(probe);
  }
  const uint16_t senderPort = ports[0];
  const uint16_t heardPort = ports[1];
  nr_transport_addr senderAddr = UdpAddr(kDeclared, senderPort);
  nr_transport_addr heardAddr = UdpAddr(kDeclared, heardPort);
  RefPtr<NrSocket> sender = new NrSocket();
  RefPtr<NrSocket> heard = new NrSocket();
  int senderResult = -1;
  int heardResult = -1;
  OnSocketThread([&] {
    senderResult = sender->create(&senderAddr);
    heardResult = heard->create(&heardAddr);
  });
  ASSERT_EQ(0, senderResult);
  ASSERT_EQ(0, heardResult);

  // The control: the port of a socket that was born IS one of ours, a datagram to it goes to the loopback and the socket hears it.
  int sentToHeard = -1;
  OnSocketThread([&] { sentToHeard = sender->sendto("ping", 4, 0, &heardAddr); });
  EXPECT_EQ(0, sentToHeard);
  bool heardIt = false;
  for (int attempt = 0; attempt < 50 && !heardIt; ++attempt) {
    char buf[16];
    size_t len = 0;
    nr_transport_addr from;
    int r = -1;
    OnSocketThread([&] { r = heard->recvfrom(buf, sizeof(buf), &len, 0, &from); });
    heardIt = (r == 0 && len == 4);
    if (!heardIt) {
      PR_Sleep(PR_MillisecondsToInterval(20));
    }
  }
  EXPECT_TRUE(heardIt) << "the datagram to a port of ours did not go to the loopback: the test below would prove nothing";

  // A create whose last step fails.
  const uint16_t refusedPort = ports[2];
  nr_transport_addr refusedAddr = UdpAddr(kDeclared, refusedPort);
  RefPtr<NrSocket> refused = new NrSocket();
  int refusedResult = 0;
  StealthFailNextSocketAttachForTests(true);
  OnSocketThread([&] { refusedResult = refused->create(&refusedAddr); });
  StealthFailNextSocketAttachForTests(false);
  EXPECT_EQ(R_INTERNAL, refusedResult) << "the socket transport service refused the socket and create went on (or failed for another reason)";
  refused = nullptr;  // closes its file descriptor: the port is free again

  // Someone else takes that port. A datagram of ours to the declared address and that port must NOT be taken for a datagram to one of ours.
  PRFileDesc* other = PR_NewUDPSocket();
  PRNetAddr bindTo;
  PR_InitializeNetAddr(PR_IpAddrAny, refusedPort, &bindTo);
  ASSERT_EQ(PR_SUCCESS, PR_Bind(other, &bindTo));
  OnSocketThread([&] { (void)sender->sendto("ping", 4, 0, &refusedAddr); });
  char buf[16];
  PRNetAddr from;
  EXPECT_LT(PR_RecvFrom(other, buf, sizeof(buf), 0, &from, PR_MillisecondsToInterval(300)), 0)
      << "a port left by a create that failed is still taken for a port of ours: the datagram went to the loopback";
  PR_Close(other);
  OnSocketThread([&] {
    sender->close();
    heard->close();
  });
}

}  // namespace mozilla
