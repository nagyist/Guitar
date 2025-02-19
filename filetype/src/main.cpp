
#include "FileType.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

int main(int argc, char **argv)
{
#ifdef _WIN32
	char const *path = "../Dockerfile";
#else
	char const *path = "a.out";
#endif
	FileType ft;
	ft.open("../lib/magic.mgc");
	if (1) {
		int fd = open(path, O_RDONLY);
		if (fd != -1) {
			if (1) {
				struct stat st;
				if (fstat(fd, &st) == 0 && st.st_size > 0) {
					std::vector<char> buf(st.st_size);
					ssize_t nbytes = read(fd, buf.data(), buf.size());
					if (nbytes > 0) {
						FileType::Result r = ft.file(buf.data(), buf.size(), st.st_mode);
						puts(r.mimetype.c_str());
					}
				}
				close(fd);
			} else {
				FileType::Result r = ft.file(fd);
				puts(r.mimetype.c_str());
			}
		}
	} else {
		FileType::Result r = ft.file(path);
		puts(r.mimetype.c_str());
	}
	return 0;
}
