/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The language of a document's text that declares none is the language of its
// context. Document::mLanguageFromCharset is EncodingToLang's answer, which for
// an encoding without a language of its own is the process's locale language;
// DocumentCharsetLanguage replaces that answer, and only that answer, with the
// language the context declares (or, with no context, the session's).

#include <thread>

#include "gtest/gtest.h"
#include "js/RealmOptions.h"
#include "mozilla/Preferences.h"
#include "mozilla/dom/StealthSessionDeclaration.h"
#include "mozilla/intl/StealthSessionPrefs.h"
#include "nsGkAtoms.h"
#include "nsLanguageAtomService.h"
#include "nsString.h"

using namespace mozilla;
using namespace mozilla::dom;

namespace {
// The session's declaration, as the core writes it, for the life of a test.
struct DeclaredLanguage {
  explicit DeclaredLanguage(const char* aList) {
    Preferences::SetCString("juggler.locale.override", aList);
  }
  ~DeclaredLanguage() { Preferences::ClearUser("juggler.locale.override"); }
};
}  // namespace

TEST(StealthDocumentLanguage, AnEncodingWithoutALanguageTakesTheDeclaredOne)
{
  RefPtr<nsAtom> process = NS_Atomize("en-us");
  RefPtr<nsAtom> declared = NS_Atomize("ja");
  // UTF-8 and the encodings like it: Lookup answered the process language.
  EXPECT_EQ(ChooseCharsetLanguage(process, process, declared), declared.get());
}

TEST(StealthDocumentLanguage, AnEncodingWithALanguageOfItsOwnKeepsIt)
{
  RefPtr<nsAtom> process = NS_Atomize("en-us");
  RefPtr<nsAtom> declared = NS_Atomize("ja");
  // windows-1251 is Cyrillic whatever the context says.
  EXPECT_EQ(ChooseCharsetLanguage(nsGkAtoms::x_cyrillic, process, declared),
            nsGkAtoms::x_cyrillic);
}

TEST(StealthDocumentLanguage, NothingDeclaredChangesNothing)
{
  RefPtr<nsAtom> process = NS_Atomize("en-us");
  EXPECT_EQ(ChooseCharsetLanguage(process, process, nullptr), process.get());
  EXPECT_EQ(ChooseCharsetLanguage(nullptr, process, nullptr), nullptr);
}

TEST(StealthDocumentLanguage, TheDeclaredListBecomesTheAtomOfItsPrimaryTag)
{
  nsAtom* ja = InternedLanguage("ja-JP, ja, en-US, en"_ns);
  ASSERT_TRUE(ja);
  nsAutoCString tag;
  ja->ToUTF8String(tag);
  EXPECT_TRUE(tag.EqualsLiteral("ja-jp"));  // lower case, like every language atom
  // The same list gives the same atom: a Document holds the pointer, so the
  // atom must be one and the same for the life of the process.
  EXPECT_EQ(InternedLanguage("ja-JP"_ns), ja);
  EXPECT_EQ(InternedLanguage("  ja-JP ,en"_ns), ja);
  EXPECT_NE(InternedLanguage("de-DE"_ns), ja);
  // And asking in another order does not mix the answers up (the last answer
  // is remembered).
  EXPECT_EQ(InternedLanguage("ja-JP, ja, en-US, en"_ns), ja);
}

TEST(StealthDocumentLanguage, NothingDeclaredIsNoAtom)
{
  EXPECT_EQ(InternedLanguage(""_ns), nullptr);
  EXPECT_EQ(InternedLanguage("  , "_ns), nullptr);
}

// With no Document at all (so no context) the session's declaration decides.

TEST(StealthDocumentLanguage, WithNoContextTheSessionsLanguageIsTheDocumentsOne)
{
  nsAtom* process = nsLanguageAtomService::GetService()->GetLocaleLanguage();
  {
    DeclaredLanguage declared("ko-KR, ko, en-US, en");
    nsAtom* got = DocumentCharsetLanguage(nullptr, process);
    nsAutoCString tag;
    got->ToUTF8String(tag);
    EXPECT_TRUE(tag.EqualsLiteral("ko-kr")) << tag.get();
    // An encoding with a language of its own keeps it.
    EXPECT_EQ(DocumentCharsetLanguage(nullptr, nsGkAtoms::x_cyrillic),
              nsGkAtoms::x_cyrillic);
  }
  // Nothing declared: the process's answer stands.
  EXPECT_EQ(DocumentCharsetLanguage(nullptr, process), process);
}

// SetDeclaredRealmTimeAndLanguage on a thread that is not the main one: the
// thread's own language, set, replaced and cleared.

TEST(StealthDocumentLanguage, ARealmCreatedOnAWorkerThreadDeclaresTheThreadsLanguage)
{
  nsAtom* first = nullptr;
  nsAtom* second = nullptr;
  nsAtom* cleared = reinterpret_cast<nsAtom*>(1);
  nsAtom* rfp = reinterpret_cast<nsAtom*>(1);
  std::thread worker([&] {
    JS::RealmOptions options;
    SetDeclaredRealmTimeAndLanguage(options, false, false, "ja-JP, ja"_ns,
                                    u""_ns);
    first = intl::WorkerLocaleLanguage();
    // A second realm on the same thread (a worklet's) with another language,
    SetDeclaredRealmTimeAndLanguage(options, false, false, "de-DE"_ns, u""_ns);
    second = intl::WorkerLocaleLanguage();
    // then with none at all: the thread keeps nothing of the first.
    SetDeclaredRealmTimeAndLanguage(options, false, false, ""_ns, u""_ns);
    cleared = intl::WorkerLocaleLanguage();
    SetDeclaredRealmTimeAndLanguage(options, false, false, "ko-KR"_ns, u""_ns);
    // and the spoofed locale of resistFingerprinting is not a language.
    SetDeclaredRealmTimeAndLanguage(options, false, true, "ko-KR"_ns, u""_ns);
    rfp = intl::WorkerLocaleLanguage();
  });
  worker.join();
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  nsAutoCString tag;
  first->ToUTF8String(tag);
  EXPECT_TRUE(tag.EqualsLiteral("ja-jp"));
  second->ToUTF8String(tag);
  EXPECT_TRUE(tag.EqualsLiteral("de-de"));
  EXPECT_EQ(cleared, nullptr);
  EXPECT_EQ(rfp, nullptr);
  // The main thread never gets a worker language.
  EXPECT_EQ(intl::WorkerLocaleLanguage(), nullptr);
}
