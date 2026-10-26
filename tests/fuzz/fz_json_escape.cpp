// fz_json_escape.cpp - fuzz webui_json jsonEscape (surface #3/#8: JSON string
// writer / thread-name escaping). Invariant: output must be a structurally valid
// JSON string body (balanced escapes, no raw control chars).
#include <string>
#include "fuzz_engine.h"

// provided by webui_json.cpp (extern, no namespace in TU)
std::string jsonEscape(const std::string& s);

int main(int argc, char** argv) {
  const char* crashDir = argc > 1 ? argv[1] : "crash";
  const char* logTag   = argc > 2 ? argv[2] : "fz_json_escape";
  fz::Options opt;
  opt.name = logTag;
  opt.crashDir = crashDir;
  opt.minIter = 20000000ULL;
  opt.minSeconds = 1500.0;
  opt.seed = 0xC0FFEE ^ 0x9;
  opt.maxLen = 8192;
  opt.seeds = {
    "RenderThread", "a\"b", "a\\b", "a\nb", "a\tb", "中文线程名",
    "\x01\x02\x7f", "pkg/com.foo.Activity", "line1\nline2\t\"quoted\"",
  };
  fz::run(opt, [](const std::string& in) {
    std::string out = jsonEscape(in);
    // invariant: no raw control chars may remain in output; every backslash
    // must be followed by a valid escape char.
    for (size_t i = 0; i < out.size(); ++i) {
      unsigned char c = (unsigned char)out[i];
      if (c == '\\') {
        if (i + 1 >= out.size()) { fprintf(stderr, "unescaped trailing backslash\n"); abort(); }
        char e = out[i + 1];
        if (e != '"' && e != '\\' && e != '/' && e != 'b' && e != 'f' &&
            e != 'n' && e != 'r' && e != 't' && e != 'u') {
          fprintf(stderr, "invalid escape \\%c\n", e); abort();
        }
        ++i;
      } else if (c < 0x20) {
        fprintf(stderr, "raw control char 0x%02x in escaped output\n", c); abort();
      }
    }
  });
  return 0;
}
