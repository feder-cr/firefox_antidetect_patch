# fingerprint/media/speech-voices

Category: spoof
Prefs: zoom.stealth.voices.list
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/media/webspeech/synth/SpeechSynthesis.cpp` (SpeechSynthesis::GetVoices (block before nsSynthVoiceRegistry::GetVoiceCount), movable): When the pref is non-empty the OS voice registry is never consulted, so the list replaces the real voices instead of overriding them. The NAME|LANG|D|L parsing and the stealth-voice: URI construction live in Mozilla code; move them to our file behind one call. Synthetic voices are never registered with the OS. [verified: CONFIRMED functional gap, not a crash: SpeakUtterance passes voice->GetVoiceU
- `dom/media/webspeech/synth/SpeechSynthesisVoice.cpp` (file-scope IsStealthVoiceURI and GetStealthVoiceField (new, top of file); SpeechSynthesisVoice::GetVoiceURI, GetName, GetLang, LocalService, Default, movable): The accessors each have a 2-4 line prefix that is already a hook. The parser (about 25 lines) and the GetVoiceURI rewrite (about 8 lines) should move to our file. GetVoiceURI now returns the urn:moz-tts:sapi: scheme on every platform, including Linux and macOS builds, by design (per the comment). The GetVoiceURI comment is a long Italian narrative with measurement dates; translate and trim it, or 
