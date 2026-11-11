#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static bool
compile_file(const char *path, const char *entry, const char *target, const char *out, std::string &error) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    error = "cannot open source";
    return false;
  }
  std::vector<char> src;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) src.insert(src.end(), buf, buf + n);
  fclose(f);
  ID3DBlob *code = nullptr, *err = nullptr;
  HRESULT hr = D3DCompile(src.data(), src.size(), path, nullptr, nullptr, entry, target, 0, 0, &code, &err);
  if (err) error.assign((const char *)err->GetBufferPointer(), err->GetBufferSize());
  if (FAILED(hr)) return false;
  FILE *o = fopen(out, "wb");
  fwrite(code->GetBufferPointer(), 1, code->GetBufferSize(), o);
  fclose(o);
  return true;
}

int main(int argc, char **argv) {
  // -b jobs.txt: one "source<TAB>entry<TAB>target<TAB>output" per line, compiled in this one process;
  // a failed job leaves its message in <output>.err
  if (argc == 3 && !strcmp(argv[1], "-b")) {
    FILE *jobs = fopen(argv[2], "r");
    if (!jobs) return 3;
    char line[4096];
    int failed = 0;
    while (fgets(line, sizeof(line), jobs)) {
      line[strcspn(line, "\r\n")] = 0;
      char *fields[4];
      char *cursor = line;
      int count = 0;
      for (; count < 4 && cursor; count++) {
        fields[count] = cursor;
        cursor = strchr(cursor, '\t');
        if (cursor) *cursor++ = 0;
      }
      if (count != 4) continue;
      std::string error;
      if (!compile_file(fields[0], fields[1], fields[2], fields[3], error)) {
        std::string err_path = std::string(fields[3]) + ".err";
        FILE *e = fopen(err_path.c_str(), "w");
        fputs(error.empty() ? "compile failed" : error.c_str(), e);
        fclose(e);
        failed++;
      }
    }
    fclose(jobs);
    return failed ? 1 : 0;
  }
  if (argc == 3 && !strcmp(argv[1], "-d")) {
    FILE *f = fopen(argv[2], "rb");
    if (!f) return 3;
    std::vector<char> code;
    char b[4096];
    size_t m;
    while ((m = fread(b, 1, sizeof(b), f)) > 0) code.insert(code.end(), b, b + m);
    fclose(f);
    ID3DBlob *text = nullptr;
    if (FAILED(D3DDisassemble(code.data(), code.size(), 0, nullptr, &text))) return 1;
    printf("%s\n", (const char *)text->GetBufferPointer());
    return 0;
  }
  if (argc < 5) {
    fprintf(stderr, "usage: hlslc <file.hlsl> <entry> <target> <out>\n");
    return 2;
  }
  FILE *f = fopen(argv[1], "rb");
  if (!f) return 3;
  std::vector<char> src;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) src.insert(src.end(), buf, buf + n);
  fclose(f);
  ID3DBlob *code = nullptr, *err = nullptr;
  HRESULT hr = D3DCompile(src.data(), src.size(), argv[1], nullptr, nullptr, argv[2], argv[3], 0, 0, &code, &err);
  if (err) fprintf(stderr, "%.*s\n", (int)err->GetBufferSize(), (const char *)err->GetBufferPointer());
  if (FAILED(hr)) return 1;
  FILE *o = fopen(argv[4], "wb");
  fwrite(code->GetBufferPointer(), 1, code->GetBufferSize(), o);
  fclose(o);
  return 0;
}
