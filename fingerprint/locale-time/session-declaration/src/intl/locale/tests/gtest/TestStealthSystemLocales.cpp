/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The languages "the machine" has are the session's declaration, not the
// operating system's: OSPreferences::GetSystemLocales answers from
// SessionSystemLocales when a language is declared. The order of the CJK
// fallback fonts (gfxPlatformFontList::FontPrefs) is negotiated against this
// list, so a host whose user list starts with Japanese must not put Japanese
// first for a session that declares en-US.

#include <initializer_list>
#include <thread>

#include "gtest/gtest.h"
#include "mozilla/Preferences.h"
#include "mozilla/gtest/MozAssertions.h"
#include "mozilla/intl/OSPreferences.h"
#include "mozilla/intl/StealthSessionPrefs.h"
#include "nsString.h"
#include "nsTArray.h"

using namespace mozilla::intl;

static nsTArray<nsCString> Declared(const char* aList, bool* aDeclared = nullptr) {
  nsTArray<nsCString> locales;
  locales.AppendElement("stale"_ns);  // a previous answer is never kept
  bool declared = SystemLocalesFromDeclared(nsDependentCString(aList), locales);
  if (aDeclared) {
    *aDeclared = declared;
  }
  return locales;
}

namespace {
// The session's declaration for the life of a test: an ASSERT that returns early still clears it.
struct DeclaredLanguage {
  explicit DeclaredLanguage(const char* aList) {
    Preferences::SetCString("juggler.locale.override", aList);
  }
  ~DeclaredLanguage() { Preferences::ClearUser("juggler.locale.override"); }
};
}  // namespace

static void ExpectLocales(const nsTArray<nsCString>& aGot,
                          std::initializer_list<const char*> aWant) {
  ASSERT_EQ(aGot.Length(), aWant.size());
  size_t i = 0;
  for (const char* want : aWant) {
    EXPECT_TRUE(aGot[i].EqualsASCII(want)) << aGot[i].get() << " != " << want;
    ++i;
  }
}

TEST(Intl_Locale_StealthSystemLocales, TheDeclaredListIsTheListOfTheMachine)
{
  bool declared = false;
  auto locales = Declared("it-IT, it, en-US, en", &declared);
  EXPECT_TRUE(declared);
  // A machine lists the languages its user chose, not each language bare as
  // well: "it" and "en" are stood for by it-IT and en-US.
  ExpectLocales(locales, {"it-IT", "en-US"});
}

TEST(Intl_Locale_StealthSystemLocales, TheOrderIsTheDeclaredOrder)
{
  ExpectLocales(Declared("ja-JP, ja, en-US, en"), {"ja-JP", "en-US"});
  ExpectLocales(Declared("en-US, en, ja-JP, ja"), {"en-US", "ja-JP"});
}

TEST(Intl_Locale_StealthSystemLocales, ABareLanguageNothingStandsForStays)
{
  ExpectLocales(Declared("en, fr"), {"en", "fr"});
  ExpectLocales(Declared("zh-TW, zh, ja"), {"zh-TW", "ja"});
}

TEST(Intl_Locale_StealthSystemLocales, QualityValuesBlanksAndRepeatsGo)
{
  ExpectLocales(Declared(" de-DE ;q=0.9, ,de-DE, fr-FR;q=0.5 ,"),
                {"de-DE", "fr-FR"});
}

TEST(Intl_Locale_StealthSystemLocales, NothingDeclaredIsMozillasPath)
{
  bool declared = true;
  auto locales = Declared("", &declared);
  EXPECT_FALSE(declared);
  EXPECT_TRUE(locales.IsEmpty());  // the caller then asks the OS, as upstream

  declared = true;
  locales = Declared(" , ;q=1, ", &declared);
  EXPECT_FALSE(declared);
  EXPECT_TRUE(locales.IsEmpty());
}

TEST(Intl_Locale_StealthSystemLocales, WithoutADeclarationTheSystemAnswers)
{
  // The gtest process declares no language (juggler.locale.override is
  // empty), so the hook in OSPreferences::GetSystemLocales steps aside and
  // the answer is the platform's: at least one locale, as upstream's test
  // of the same method asks.
  nsTArray<nsCString> declared;
  EXPECT_FALSE(SessionSystemLocales(declared));
  EXPECT_TRUE(declared.IsEmpty());

  nsTArray<nsCString> system;
  ASSERT_TRUE(NS_SUCCEEDED(
      OSPreferences::GetInstance()->GetSystemLocales(system)));
  EXPECT_FALSE(system.IsEmpty());
}

// The language of a worker thread's text is its context's.

TEST(Intl_Locale_StealthSystemLocales, TheMainThreadHasNoWorkerLanguage)
{
  EXPECT_EQ(WorkerLocaleLanguage(), nullptr);
}

TEST(Intl_Locale_StealthSystemLocales, AWorkerThreadKeepsItsOwnDeclaredLanguage)
{
  nsAtom* seenOnJa = nullptr;
  nsAtom* seenOnDe = nullptr;
  nsAtom* seenOnBare = reinterpret_cast<nsAtom*>(1);
  std::thread ja([&] {
    SetWorkerLocaleLanguage("ja-JP, ja, en-US, en"_ns);
    seenOnJa = WorkerLocaleLanguage();
  });
  ja.join();
  std::thread de([&] {
    SetWorkerLocaleLanguage("de-DE"_ns);
    seenOnDe = WorkerLocaleLanguage();
  });
  de.join();
  std::thread bare([&] { seenOnBare = WorkerLocaleLanguage(); });
  bare.join();

  ASSERT_TRUE(seenOnJa);
  ASSERT_TRUE(seenOnDe);
  nsAutoCString tag;
  seenOnJa->ToUTF8String(tag);
  EXPECT_TRUE(tag.EqualsLiteral("ja-jp"));
  seenOnDe->ToUTF8String(tag);
  EXPECT_TRUE(tag.EqualsLiteral("de-de"));
  // A thread that declared nothing has no worker language, and neither has
  // the main thread, whatever the workers declared.
  EXPECT_EQ(seenOnBare, nullptr);
  EXPECT_EQ(WorkerLocaleLanguage(), nullptr);
}

TEST(Intl_Locale_StealthSystemLocales, TheWorkerLanguageAtomOutlivesItsThread)
{
  nsAtom* kept = nullptr;
  std::thread worker([&] {
    SetWorkerLocaleLanguage("ko-KR"_ns);
    kept = WorkerLocaleLanguage();
  });
  worker.join();
  ASSERT_TRUE(kept);
  nsAutoCString tag;
  kept->ToUTF8String(tag);  // the thread is gone: the atom must still be alive
  EXPECT_TRUE(tag.EqualsLiteral("ko-kr"));
}

// The declaration is read when the locales are ASKED, never only when the
// host is read: a first read before the preferences are ready fills the cache
// of OSPreferences with the host's list, and the declaration that arrives
// afterwards must still win, for GetSystemLocales and for GetSystemLocale.

TEST(Intl_Locale_StealthSystemLocales, TheDeclarationWinsOverACacheFilledBeforeIt)
{
  OSPreferences* os = OSPreferences::GetInstance();
  nsTArray<nsCString> before;
  ASSERT_TRUE(NS_SUCCEEDED(os->GetSystemLocales(before)));  // nothing declared: the platform's, now cached
  nsTArray<nsCString> cached;
  ASSERT_TRUE(NS_SUCCEEDED(os->GetSystemLocales(cached)));
  ASSERT_FALSE(cached.IsEmpty());

  DeclaredLanguage session("ja-JP, ja, en-US, en");
  nsTArray<nsCString> declared;
  ASSERT_TRUE(NS_SUCCEEDED(os->GetSystemLocales(declared)));
  ExpectLocales(declared, {"ja-JP", "en-US"});
  nsAutoCString first;
  EXPECT_TRUE(NS_SUCCEEDED(os->GetSystemLocale(first)));
  EXPECT_TRUE(first.EqualsLiteral("ja-JP"));

  // And when the declaration goes away the platform answers again.
  Preferences::ClearUser("juggler.locale.override");
  nsTArray<nsCString> back;
  ASSERT_TRUE(NS_SUCCEEDED(os->GetSystemLocales(back)));
  EXPECT_EQ(back, before);
}

TEST(Intl_Locale_StealthSystemLocales, TheDeclaredListIsWhatEveryCallerOfTheSystemSees)
{
  OSPreferences* os = OSPreferences::GetInstance();
  nsTArray<nsCString> before;
  ASSERT_TRUE(NS_SUCCEEDED(os->GetSystemLocales(before)));

  DeclaredLanguage session("ko-KR, ko, en-US, en");
  os->AssignSysLocales(nsTArray<nsCString>());  // an empty cache: the next read could go to the platform
  nsTArray<nsCString> declared;
  ASSERT_TRUE(NS_SUCCEEDED(os->GetSystemLocales(declared)));
  ExpectLocales(declared, {"ko-KR", "en-US"});
  nsAutoCString first;
  EXPECT_TRUE(NS_SUCCEEDED(os->GetSystemLocale(first)));
  EXPECT_TRUE(first.EqualsLiteral("ko-KR"));

  Preferences::ClearUser("juggler.locale.override");
  os->AssignSysLocales(before);  // leave the process as it was
}

TEST(Intl_Locale_StealthSystemLocales, TheFirstOfTheDeclaredListAsOneTag)
{
  nsAutoCString tag;
  EXPECT_FALSE(SessionSystemLocale(tag));  // nothing declared
  DeclaredLanguage session("fr-CA, fr, en-US, en");
  EXPECT_TRUE(SessionSystemLocale(tag));
  EXPECT_TRUE(tag.EqualsLiteral("fr-CA"));
}
