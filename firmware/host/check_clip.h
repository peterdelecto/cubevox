// Two synthetic voiced clips for the headless check. Adam's clips in samples/ are private and stay
// out of the repository, so a checkout without them (CI) plays these instead. Each clip is a
// vowel-like voice, harmonics under formant peaks with a 5 Hz vibrato and a once-a-second level
// swell, written as a mono 16-bit wav.

#pragma once

#include <string>

namespace host {

// Writes `seconds` of a voice at `f0Hz` to `path`. False when the file cannot be written.
bool writeVoicedClip(const std::string& path, double f0Hz, double seconds);

// A temporary directory that holds the check's two clips. The destructor removes both files and the
// directory.
class SyntheticClips {
 public:
  static constexpr const char* kFirstName = "cubevox-check-voice-low.wav";
  static constexpr const char* kSecondName = "cubevox-check-voice-high.wav";

  SyntheticClips();
  ~SyntheticClips();
  SyntheticClips(const SyntheticClips&) = delete;
  SyntheticClips& operator=(const SyntheticClips&) = delete;

  // False, with both paths empty, when the directory or a clip could not be written.
  bool ok() const { return !first_.empty(); }
  const std::string& dir() const { return dir_; }
  const std::string& first() const { return first_; }
  const std::string& second() const { return second_; }

 private:
  std::string dir_;
  std::string first_;
  std::string second_;
};

}  // namespace host
