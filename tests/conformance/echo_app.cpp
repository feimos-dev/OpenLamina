// A program for the applications case to launch (tests/unit/data/echo.vrmanifest): writes its arguments into the
// file named by its first argument, and exits. With "--stay <seconds>" it then stays running that long first.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

int main(int argc, char** argv)
{
	if (argc < 2)
		return 2;
	FILE* f = fopen(argv[1], "w");
	if (!f)
		return 1;
	double stay = 0;
	bool first = true;
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--stay") && i + 1 < argc) {
			stay = atof(argv[++i]);
			continue;
		}
		fprintf(f, "%s%s", first ? "" : " ", argv[i]);
		first = false;
	}
	fprintf(f, "\n");
	fclose(f);
	if (stay > 0)
		std::this_thread::sleep_for(std::chrono::milliseconds((long long)(stay * 1000)));
	return 0;
}
