// Speaks the line protocol of tests/difftest.py; every
// request is answered by machine-translated code from gen/spike.c. Objects are
// laid out as raw bytes at the offsets the original uses.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "runtime.h"

extern "C" {
void f_140440660(cpu_t *), f_140440790(cpu_t *), f_1401e3be0(cpu_t *), f_1401e3210(cpu_t *);
void f_1401eba10(cpu_t *);
void f_1401eecc0(cpu_t *), f_1401eea40(cpu_t *), f_1401ee8d0(cpu_t *);
void f_1401ef260(cpu_t *), f_1401eefa0(cpu_t *), f_1401eee30(cpu_t *);
}

namespace {

uint64_t ReadU64(std::istream& in) { std::string s; in >> s; return std::stoull(s, nullptr, 16); }
double ReadDouble(std::istream& in) { uint64_t b = ReadU64(in); double v; std::memcpy(&v, &b, 8); return v; }
void PrintU64(uint64_t v) { std::printf("%016" PRIx64 " ", v); }
void PrintMem(const void* p, int qwords) {
    for (int i = 0; i < qwords; ++i) { uint64_t v; std::memcpy(&v, static_cast<const char*>(p) + 8 * i, 8); PrintU64(v); }
}
uint64_t Addr(const void* p) { return reinterpret_cast<uint64_t>(p); }

struct Series { uint64_t terms; int32_t count; int32_t pad; };

struct Orbit {
    alignas(16) uint8_t object[0xa0];
    std::vector<std::vector<double>> terms;
    std::vector<Series> series[3];

    explicit Orbit(uint64_t vtable) { std::memset(object, 0xee, sizeof object); std::memcpy(object, &vtable, 8); }

    void Load(std::istream& in) {
        int counts[3];
        in >> counts[0] >> counts[1] >> counts[2];
        terms.reserve(static_cast<size_t>(counts[0] + counts[1] + counts[2]));
        for (int c = 0; c < 3; ++c) {
            for (int k = 0; k < counts[c]; ++k) {
                int n; in >> n;
                auto& list = terms.emplace_back();
                for (int i = 0; i < 3 * n; ++i) list.push_back(ReadDouble(in));
                series[c].push_back({Addr(list.data()), n, 0});
            }
            uint64_t p = Addr(series[c].data());
            std::memcpy(object + 0x78 + 8 * c, &p, 8);
            std::memcpy(object + 0x90 + 4 * c, &counts[c], 4);
        }
    }
};

void RunOrbit(const std::string& what, std::istream& in, uint64_t vtable,
              recomp_fn init, recomp_fn position, recomp_fn velocity) {
    Orbit orbit(vtable);
    cpu_t c{};
    c.r[RCX] = Addr(orbit.object);
    if (what == "init") {
        recomp_invoke(&c, init);
        PrintMem(orbit.object + 8, 4 + 9);
        return;
    }
    uint64_t time[2] = {ReadU64(in), 0};
    time[1] = ReadU64(in);
    orbit.Load(in);
    double out[3];
    c.r[RDX] = Addr(out);
    c.r[R8] = Addr(time);
    recomp_invoke(&c, what == "pos" ? position : velocity);
    PrintMem(out, 3);
}

}  // namespace

int main(int argc, char** argv) {
    recomp_set_reserved_range(recomp_reserved_range, RECOMP_RESERVED_SIZE);
    if (argc < 2 || recomp_map_image(argv[1]) != 0) {
        std::fprintf(stderr, "usage: driver <path to SpaceEngine.exe>\n");
        return 2;
    }
    { cpu_t c{}; recomp_invoke(&c, f_1401e3210); }  // the original's own time-constant initializer

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string command;
        in >> command;
        cpu_t c{};
        if (command == "from_days") {
            uint64_t out[2];
            c.r[RCX] = Addr(out);
            c.x[1].f64[0] = ReadDouble(in);
            recomp_invoke(&c, f_140440660);
            PrintMem(out, 2);
        } else if (command == "to_days") {
            uint64_t t[2] = {ReadU64(in), 0};
            t[1] = ReadU64(in);
            c.r[RCX] = Addr(t);
            recomp_invoke(&c, f_140440790);
            PrintU64(c.x[0].q[0]);
        } else if (command == "date") {
            int year, month, day, hour, minute;
            in >> year >> month >> day >> hour >> minute;
            uint8_t date[16];
            std::memcpy(date, &year, 4);
            date[4] = static_cast<uint8_t>(month), date[5] = static_cast<uint8_t>(day);
            date[6] = static_cast<uint8_t>(hour), date[7] = static_cast<uint8_t>(minute);
            double second = ReadDouble(in);
            std::memcpy(date + 8, &second, 8);
            uint64_t out[2];
            c.r[RCX] = Addr(date);
            c.r[RDX] = Addr(out);
            recomp_invoke(&c, f_1401e3be0);
            PrintMem(out, 2);
        } else if (command == "series") {
            double T = ReadDouble(in);
            int n; in >> n;
            std::vector<double> terms;
            for (int i = 0; i < 3 * n; ++i) terms.push_back(ReadDouble(in));
            terms.push_back(0);  // keep data() non-null for an empty series
            Series s{Addr(terms.data()), n, 0};
            c.r[RCX] = Addr(&s);
            c.x[1].f64[0] = T;
            recomp_invoke(&c, f_1401eba10);
            PrintU64(c.x[0].q[0]);
        } else if (command.rfind("rect_", 0) == 0) {
            RunOrbit(command.substr(5), in, 0x1406895b0, f_1401eecc0, f_1401eea40, f_1401ee8d0);
        } else if (command.rfind("polar_", 0) == 0) {
            RunOrbit(command.substr(6), in, 0x1406895d8, f_1401ef260, f_1401eefa0, f_1401eee30);
        } else {
            std::printf("unknown command");
        }
        std::printf("\n");
    }
    return 0;
}
