#include <iostream>
#include <sstream>
#include <string>
#include <cstdint>
#include <vector>
#include <filesystem>
#include <cstring>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <getopt.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif
#endif

#include "lodepng.h"

using namespace std;

#define MAX_OFFSETS 84
#define NAME_SIZE 64
#define SPLASH_HDR_SIZE 4096

#define OP_MAGIC_V1 "SPLASH!!"

#define ROUND_UP(n) ((((n >> 12)) << 12) == n) ? n : (((n >> 12) + 1) << 12)

struct config {
    char *outdir;
    char *imgpath;
    char *outpath;
    bool decrypt;
} cfg;

typedef struct 
{
    uint8_t     magic[8];
    uint8_t     blank[24];
    uint32_t    width;
    uint32_t    height;
    uint32_t    length;
    uint32_t    special;
    uint32_t    offsets[MAX_OFFSETS];
    uint8_t     name[NAME_SIZE];
    uint8_t     metadata[3648];
} op6t_splash_hdr;


std::vector<uint8_t> raw2rle(std::vector<uint8_t> &data)
{
    std::vector<uint8_t> rle_data;

    uint8_t last = 0;
    uint8_t count = 0;
    auto length = data.size();

    for (size_t i=0; i<length; i++) {
	    if (i == 0) {
	    	last = data[i];
	    	count++;
	    } else {
	    	if (data[i] == last) {
	    		if (count == 255) {
	    			rle_data.push_back(last);
	    			rle_data.push_back(count);
	    			count = 1;
	    		} else {
	    			count++;
	    		}
	    	} else {
	    		rle_data.push_back(last);
	    		rle_data.push_back(count);
	    		count = 1;
	    	}
	    	if (i == length-1) {
	    		last = data[i];
	    		rle_data.push_back(last);
	    		rle_data.push_back(count);
	    	}
	    }
	    last = data[i];
    }
    return rle_data;
}

std::vector<uint8_t> rle2raw(std::vector<uint8_t> &data)
{
	std::vector<uint8_t> raw_data;
	
    uint8_t sz = 0;
	uint8_t byte = 0;
	for (size_t i = 0; i < data.size(); i++) {
		if ((i&1) == 1) {
			sz = data[i];
			for (uint8_t j = 0; j < sz; j++) {
				raw_data.push_back(byte);
			}
		} else {
			byte = data[i];
		}
	}
	return raw_data;
}

void RGB2BGR(vector<uint8_t> &data) {
    if (data.size() % 3) {
        return;
    }

    uint8_t tmp;
    for (size_t i=0; i<data.size(); i+=3) {
        tmp = data[i];
        data[i] = data[i+2];
        data[i+2] = tmp;
    }
}

class OP6TLOGO 
{
private:

    int fd = 0;

    void *map = NULL;
    size_t fsize;

    op6t_splash_hdr hdr;
#ifdef _WIN32
    HANDLE dumpFileDescriptor;
    HANDLE fileMappingObject;
#endif

public:
    int etype = 0;

    OP6TLOGO(const char *logoimg)
    {
        if (!logoimg) {
            etype = EINVAL;
            return;
        }

        if (!(access(logoimg, F_OK) == 0)) {
            etype = EEXIST;
            return;
        }

        struct stat st;
        stat(logoimg, &st);

        fsize = st.st_size;
        
        // parse image
#ifdef _WIN32
        dumpFileDescriptor = CreateFileA(logoimg,
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              NULL);
        fileMappingObject = CreateFileMapping(dumpFileDescriptor,
                              NULL,
                              PAGE_READWRITE,
                              0,
                              0,
                              NULL);
        map = MapViewOfFile(fileMappingObject,
                              FILE_MAP_ALL_ACCESS,
                              0,
                              0,
                              fsize);
#else
        fd = open(logoimg, O_RDWR | O_BINARY, 0755);
        if (!fd) {
            etype = EIO;
            return;
        }
        map = mmap(NULL,
                   fsize,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED,
                   fd,
                   0);
#endif
        if (!map) {
            cerr << "Error: Cannot open file via mmap!" << endl;
            etype = EIO;
            return;
        }

        (void) parse_img();

    }
    ~OP6TLOGO()
    {
        if (fd)
            close(fd);
    
        // close map
#ifdef _WIN32
        CloseHandle(dumpFileDescriptor);
        CloseHandle(fileMappingObject);
        UnmapViewOfFile(map);
#else
        munmap(map, fsize);
#endif
    }

    void parse_img() {
        memcpy(&hdr, map, sizeof(hdr));

        if (memcmp(hdr.magic, OP_MAGIC_V1, sizeof(OP_MAGIC_V1)-1) != 0) {
            cerr << "Error : Invalid image!" << endl;
            etype = EINVAL;
            return;
        }

        if (hdr.special != 1) {
            cerr << "Error : Unsupport splash format." << endl;
            etype = ENOTSUP;
            return;
        }
    }

    int unpack(const char *outdir)
    {
        if (etype) {
            cerr << "Error: " << strerror(etype) << endl;
            return etype;
        }
        int retval = 0;
        vector<uint8_t> rledata, rawdata, bufdata;
        uint8_t *m = (uint8_t*)map, *d;
        op6t_splash_hdr *h;

        uint32_t lodepng_err;

        string out = outdir, filestr;
        filesystem::path outpath = out, filepath;

        if (!(filesystem::exists(outpath) && filesystem::is_directory(outpath))) {
            if (!filesystem::create_directories(outpath)) {
                cerr << "Could not create directory: " << outpath << endl;
                etype = EIO;
                return etype;
            }
        }

        for (int i=0; i<MAX_OFFSETS; i++) {
            m = (uint8_t*)map + hdr.offsets[i];
            h = (op6t_splash_hdr*)m;
            d = m + sizeof(hdr);

            cout << "OFFSET:   " << uppercase << setw(8) << hex << hdr.offsets[i] << endl << dec <<
            "\t" << "SIZE:     " << h->length << endl <<
            "\t" << "WIDTH:    " << h->width << endl <<
            "\t" << "HEIGHT:   " << h->height << endl <<
            "\t" << "SPECIAL:  " << h->special << endl <<
            "\t" << "NAME:     " << h->name << endl <<
            "\t" << "METADATA: " << h->metadata << endl;

            cout << "\t\tParsing... " << flush;

            if (h->length == 0) // skip non data
            {
                cout << "- Skip." << endl;
                continue;
            }

            rledata.clear();
            rawdata.clear();
            bufdata.clear();
            rledata.resize(h->length);
            for (uint32_t j=0; j<h->length; j++) {
                rledata[j] = d[j];
            }

            rawdata = rle2raw(rledata);
            RGB2BGR(rawdata); // swap r and b channal

            filestr = reinterpret_cast<char*>(h->name);
            filepath = outpath / filestr;

            lodepng_err = lodepng::encode(bufdata, rawdata, h->width, h->height, LCT_RGB);
            if (lodepng_err) {
                cerr << endl << "Error: " << lodepng_error_text(lodepng_err) << endl;
                cerr << "rawdata sz:" << rawdata.size() << endl;
                retval = EIO;
                break;
            }
            lodepng_err = lodepng::save_file(bufdata, filepath.string() + ".png");
            if (lodepng_err) {
                cerr << endl << "Error: " << lodepng_error_text(lodepng_err) << endl;
                retval = EIO;
                break;
            }

            cout << "- Done!" << endl;

        }
        return retval;
    } 

    int repack(const char *outimg, const char *picdir)
    {
        if (etype)
            return etype;

        int retval = 0;

        filesystem::path picpath(picdir), spicpath;
        string name;

        uint8_t *m, *d;
        op6t_splash_hdr h, *hp;

        int offset = 0;

        int fdo = open(outimg, O_WRONLY | O_BINARY | O_TRUNC | O_CREAT);
        if (!fdo)
        {
            cerr << "Error: Could not create file " << outimg << endl;
            return EIO;
        }

        for (int i=0; i<MAX_OFFSETS; i++)
        {
            m = (uint8_t*)map + hdr.offsets[i];
            d = m + sizeof(hdr);
            hp = (op6t_splash_hdr*)m;

            unsigned lodepng_err;

            lodepng::State state;
            state.info_raw.colortype = LCT_RGB;

            vector<uint8_t> bufdata, rledata, rawdata;

            memcpy(&h, hp, sizeof(h));

            if (h.length == 0) {
                offset += write(fdo, &h, sizeof(h));
                continue;
            }

            rledata.clear();
            rawdata.clear();
            bufdata.clear();

            hdr.offsets[i] = offset;

            name = reinterpret_cast<char*>(hp->name);
            name += ".png";

            cout << "Parsing ... " << name << endl;

            if (filesystem::is_regular_file(picpath / name)) {
                lodepng_err = lodepng::load_file(bufdata ,(picpath / name).string());
                if (lodepng_err) {
                    cerr << "Error : " << lodepng_error_text(lodepng_err) << endl;
                    break;
                }
                h.width = 0;
                h.height = 0;
                lodepng_err = lodepng::decode(rawdata, h.width, h.height, state, bufdata);
                if (lodepng_err) {
                    cerr << "Error : " << lodepng_error_text(lodepng_err) << endl;
                    break;
                }

                RGB2BGR(rawdata);
                rledata = raw2rle(rawdata);
                h.length = rledata.size();
            } else { // Read data from origin image
                rledata.resize(h.length);
                for (uint32_t j=0; j<h.length; j++) {
                    rledata[j] = d[j];
                }
            }

            if (i == 0)
                hdr = h;


            offset += write(fdo, &h, sizeof(h));
            offset += write(fdo, rledata.data(), rledata.size());
            offset = ROUND_UP(offset);
            lseek(fdo, offset, SEEK_SET);

        }

        lseek(fdo, 0, SEEK_SET);
        write(fdo, &hdr, sizeof(hdr));

        close(fdo);

        cout << "- Done!" << endl;

        return retval;

    }

};

void print_help(void) {
    cout <<
        "op6tlogo [-i] <LOGO.img> [-o] <new-logo.img> [-p] <pic> [-d]" << endl <<
        "\t-i, --input         Input image." << endl <<
        "\t-o, --output        Output image or directory. default [" << cfg.outpath << "]" << endl <<
        "\t-p, --pic           Pic dir.                   default [" << cfg.outdir << "]" << endl <<
        "\t-d, --decrypt       Extract logo.img." << endl <<
        "\t-h, --help          Pring this help message." <<  endl <<
        "Example : " << endl <<
        "./op6tlogo -i LOGO.img -d -o pic" << endl << 
        "./op6tlogo -i LOGO.img -p pic -o new-logo.img" << endl;
}

void parse_args(int argc, char **argv) {
    const char* short_opts = "i:o:p:hd";
    const option long_opts[] = {
        {"input", required_argument, nullptr, 'i'},
        {"output", optional_argument, nullptr, 'o'},
        {"pic", optional_argument, nullptr, 'p'},
        {"help", no_argument, nullptr, 'h'},
        {"decrypt", no_argument, nullptr, 'd'},
        {nullptr, no_argument, nullptr, 0},
    };

    // init
    cfg.decrypt = false;
    cfg.imgpath = nullptr;
    cfg.outdir = strdup("pic");
    cfg.outpath = strdup("new-logo.img");

    while (true)
    {
        const auto opt = getopt_long(argc, argv, short_opts, long_opts, nullptr);

        if (-1 == opt)
            break;

        switch (opt)
        {
            case 'i':
                cfg.imgpath = strdup(optarg);
                break;
            case 'o':
                cfg.outpath = strdup(optarg);
                break;
            case 'p':
                cfg.outdir = strdup(optarg);
                break;
            case 'd':
                cfg.decrypt = true;
                break;
            case 'h':
            case '?':
            default:
                print_help();
                break;

        }
    }
}


int main(int argc, char **argv) {
    int retval = 0;

    parse_args(argc, argv);

    OP6TLOGO logo(cfg.imgpath);

    if (argc < 2)
        print_help();

    if (!cfg.imgpath) {
        cerr << "Error : image not defined!" << endl;
        retval = 1;
        goto quit;
    }

    if (logo.etype) {
        cerr << "Error : " << strerror(logo.etype) << endl;
        return logo.etype;
    }

    if (cfg.decrypt) {
        retval = logo.unpack(cfg.outdir);
    } else {
        retval = logo.repack(cfg.outpath, cfg.outdir);
    }

quit:
    if (cfg.imgpath)
        free(cfg.imgpath);
    free(cfg.outdir);
    free(cfg.outpath);

    return retval;
}