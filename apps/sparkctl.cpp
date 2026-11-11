#include "spark/strategy.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

// sparkctl quote --mid 100 --q 0 --sigma 0.1 --A 1 --k 1.5 --gamma 0.1 --xi 0.1
// Prints the GLFT quote the library actually computes. Used by the desk parity check.

static double argd(int argc, char** argv, const char* key, double def) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == key) return std::atof(argv[i + 1]);
  }
  return def;
}

int main(int argc, char** argv) {
  if (argc < 2 || std::string(argv[1]) != "quote") {
    std::fprintf(stderr, "usage: sparkctl quote --mid M --q Q --sigma S --A A --k K --gamma G --xi X\n");
    return 2;
  }
  spark::GlftParams p;
  p.gamma = argd(argc, argv, "--gamma", 0.1);
  p.xi = argd(argc, argv, "--xi", p.gamma);
  p.A = argd(argc, argv, "--A", 1);
  p.k = argd(argc, argv, "--k", 1.5);
  p.sigma = argd(argc, argv, "--sigma", 0.1);
  p.delta_t = argd(argc, argv, "--dt", 1);
  p.calibrated = false;
  const double mid = argd(argc, argv, "--mid", 100);
  const double q = argd(argc, argv, "--q", 0);
  auto o = spark::glft_quotes(mid, q, p);
  if (!o.ok) {
    std::printf("ok 0 reason %s\n", o.reason);
    return 1;
  }
  std::printf("ok 1 bid %.10f ask %.10f reservation %.10f half %.10f skew %.10f c1 %.10f c2 %.10f\n",
              o.bid, o.ask, o.reservation, o.half_spread, o.skew, o.c1, o.c2);
  return 0;
}
