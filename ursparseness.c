#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

//
//ursparse file format
//
//ursparse file format encodes data segments (meat) in a file for transfer across a wire
//holes are ignored and will be recreated on the other end implicitly when
//the data segments are written to their appropiate offsets
//
//spaces and newlines ignored in offset line
//offset length\n
//meat
//offset length\n
//meat
//...
//
//offset and length are ascii unsigned integers
//

struct ursparse {
    off_t offset;
    off_t size;
};

//parses unsigned integer
//ignores spaces before first numeral
//
//buff, sz   => input buffer to parse and its size
//
//out_sz     => output number of bytes consumed
//out_offset => output value of uint
//              valid only on 'done parsing' return value
//
//returns
//  negative number on error
//  0 done parsing, out_offset contains the parsed value
//  n intermediate parsing, 
//    need more data, 
//    out_offset contains intermediate value, 
int parse_uint(const char* buff, size_t sz, size_t* out_sz, off_t* out_n)
{
    if (!buff) return -1;
    if (!sz) return -2;
    if (!out_sz) return -3;
    if (!out_n) return -4;
    
    size_t i = 0;

    if (!*out_n) {
        //consume spaces and newlines before number
        for(; i < sz; ++i)
            if (!(buff[i] == ' ' || buff[i] == '\n' )) break;
    }

    for (; i < sz; ++i) {
        if (buff[i] >= '0' && buff[i] <= '9') {
            //numerals
            *out_n *= 10;
            *out_n += buff[i] - '0';
            continue;
        }

        if (buff[i] == ' ' || buff[i] == '\n') {
            //spaces or newline after number 
            *out_sz = i;
            return 0; //done parsing
        }

        fprintf(stderr, "ERROR: invalid char parsing offset: %c\n", buff[i]);
        return -1;
    }

    //reach EOB
    *out_sz = i;
    return i;
}

//parses until newline
//ignores spaces before newline
//any other char is an error
//
//buff, sz   => input buffer to parse and its size
//
//out_sz     => output number of bytes consumed
//
//returns
//  negative number on error
//  0 done parsing, newline was encountered and consumed
//  n intermediate parsing, 
//    need more data, 
int parse_newline(const char* buff, size_t sz, size_t* out_sz)
{
    if (!buff) return -1;
    if (!sz) return -2;
    if (!out_sz) return -3;
    
    size_t i = 0;

    //consume spaces before newline
    for(; i < sz; ++i)
        if (buff[i] != ' ') break;

    for (; i < sz; ++i) {
        if (buff[i] == '\n') {
            *out_sz = i + 1;
            return 0;
        }

        fprintf(stderr, "ERROR: invalid char parsing newline pos %ld: %c\n", i, buff[i]);
        return -1;
    }

    //reach EOB
    *out_sz = i;
    return i;
}
 
//
//writes the meat to output file
//
int do_meat(const char* buff, size_t sz, size_t* out_sz)
{
    if (!buff)   return -1;
    if (!sz)     return -2;
    if (!out_sz) return -3;

    size_t written_bytes = 0;

    while (sz > 0) {
        ssize_t r = write(1, buff, sz);

        if (-1 == r) {
            perror("ERROR: could not write to output file");
            *out_sz = written_bytes;
            return -4;
        }

        written_bytes += r;
        sz -= r;
        buff += r;
    }

    *out_sz = written_bytes;
    return written_bytes;
}

//
//writes the hole to output file
//
int do_hole(off_t offset)
{
    off_t r = lseek(1, offset, SEEK_SET);
    if ((off_t)-1 == r) {
        perror("ERROR: could not write hole to output file");
        return -1;
    }

    return 0;
}

enum ursparse_state { 
    PARSE_ERROR = -1,
    PARSE_START = 0,
    PARSE_OFFSET, 
    PARSE_SIZE,
    PARSE_NEWLINE,
    PARSE_MEAT,
};

struct ursparse_state_data {
    struct ursparse ursparse;
    enum ursparse_state state;
};

//parses ursparse line
//offset length \n
//
//buff, sz => buffer to parse and its size
//out_sz   => output number of bytes consumed
//data     => in/out parser internal state
//
//returns
//  negative number on error
//  0 done parsing ursparse line
//  n intermediate parsing
//    need more data
int parse_ursparse(const char* buff, size_t sz, size_t* out_sz, struct ursparse_state_data* data)
{
    if (!buff)   return -1;
    if (!sz)     return -2;
    if (!out_sz) return -3;
    if (!data)   return -4;

    int r = 0;
    size_t extra_sz = 0;

    switch(data->state) {

    case PARSE_START:

        data->state = PARSE_OFFSET;
        
    case PARSE_OFFSET:

        r = parse_uint(buff, sz, out_sz, &(data->ursparse.offset));
        if (r < 0) {
            fprintf(stderr, "ERROR: could not parse offset\n");
            data->state = PARSE_ERROR;
            return r;
        }

        buff += *out_sz;
        sz -= *out_sz;
        extra_sz += *out_sz;

        if (r > 0) {
            return *out_sz;
        }

        data->state = PARSE_SIZE;

    case PARSE_SIZE:
        
        r = parse_uint(buff, sz, out_sz, &(data->ursparse.size));

        if (r < 0) {
            fprintf(stderr, "ERROR: could not parse length\n");
            data->state = PARSE_ERROR;
            return r;
        }

        buff += *out_sz;
        sz -= *out_sz;
        extra_sz += *out_sz;

        if (r > 0) {
            return *out_sz;
        }

        data->state = PARSE_NEWLINE;

    case PARSE_NEWLINE:

        r = parse_newline(buff, sz, out_sz);

        if (r < 0) {
            fprintf(stderr, "ERROR: could not parse newline\n");
            data->state = PARSE_ERROR;
            return r;
        }

        buff += *out_sz;
        sz -= *out_sz;
        extra_sz += *out_sz;

        if (r > 0) {
            return *out_sz;
        }

        fprintf(stderr, "INFO: processing segment %ld %ld\n", data->ursparse.offset, data->ursparse.size);

        r = do_hole(data->ursparse.offset);
        if (r < 0) {
            data->state = PARSE_ERROR;
            return -8;
        }
        
        data->state = PARSE_MEAT;

    case PARSE_MEAT: 

        r = do_meat(buff, sz > data->ursparse.size ? data->ursparse.size : sz, out_sz);

        if (r < 0) {
            fprintf(stderr, "ERROR: could not process meat\n");
            data->state = PARSE_ERROR;
            return r;
        }

        if (data->ursparse.size < *out_sz) {
            fprintf(stderr, "ERROR: meat processor broken\n");
            data->state = PARSE_ERROR;
            return -7;
        }

        data->ursparse.size -= *out_sz;
        *out_sz += extra_sz;

        if (!data->ursparse.size) {
            //reset parsing state
            memset(data, 0, sizeof(*data));
            return 0;
        }

        return *out_sz;

    case PARSE_ERROR:
        return -5;

    default:
        return -6;
    }
}

int do_ursparse(int fd_in, int fd_out, size_t blk_sz)
{
    off_t r = lseek(fd_in, 0, SEEK_SET);
    if (r == -1) {
        perror("ERROR: output file is not seekable");
        return 1;
    }

    char* read_buff = malloc(blk_sz);
    if (!read_buff) {
        fprintf(stderr, "ERROR: could not allocate memory: %ld bytes\n", blk_sz);
        return 2;
    }

    struct ursparse_state_data data;
    memset(&data, 0, sizeof(data));

    while (1) {
        ssize_t nbytes = read(fd_in, read_buff, blk_sz);

        if (nbytes == -1) {
            perror("ERROR: could not read from input file");
            free(read_buff);
            return 3;
        }

        if (!nbytes) break; //EOF reached

        for (size_t cursor = 0; cursor < nbytes; ) {

            size_t out_sz = 0;
            int r = parse_ursparse(read_buff + cursor, nbytes - cursor, &out_sz, &data);

            if (r < 0) {
                free(read_buff);
                return 4;
            }

        cursor += out_sz;
        } 
    }

    free(read_buff);
    return 0;
}

int do_sparse_copy_data(int fd_in, int fd_out, off_t start, size_t sz)
{
    ssize_t r = copy_file_range(fd_in, &start, fd_out, 0, sz, 0);
    if (r == -1) {
        perror("ERROR: could not copy data");
        return -1;
    }
    return r;
}

int do_sparse_data(int fd_in, int fd_out, size_t blk_sz, unsigned char* hole_byte, off_t start, size_t sz)
{
    fprintf(stderr, "INFO: processing segment %ld %ld\n", start, sz);

    if (4 > dprintf(fd_out, "%ld %ld\n", start, sz)) {
        perror("ERROR: could not write segment");
        return -1;
    }
    if (-1 == do_sparse_copy_data(fd_in, fd_out, start, sz)) {
        return -1;
    }
    return 0;
}

int do_sparse(int fd_in, int fd_out, size_t blk_sz, unsigned char* hole_byte)
{
    off_t start  = lseek(fd_in, 0, SEEK_SET);
    if (start == -1) {
        perror("ERROR: input file is not seekable");
        return 1;
    }

    off_t end = 0;
    while (1) {
        start = lseek(fd_in, start, SEEK_DATA);
        if (start == -1) {
            if (errno == ENXIO) {
                 //EOF reached
                 break; 
            }
            perror("ERROR: could not seek");
            return 1;
        }
    
        end = lseek(fd_in, start, SEEK_HOLE);

        if (end == -1) {
            if (errno == ENXIO) {
                 //EOF reached
                 break; 
            }
            perror("ERROR: could not seek");
            return 1;
        }

        //printf("%ld %ld\n", start, end - start);
        if (do_sparse_data(fd_in, fd_out, blk_sz, hole_byte, start, end-start)) {
            return 2;
        }
        start = end;
    }

    if (start != -1 && end != -1) {
        //printf("%ld %ld\n", start, end - start);
        if (do_sparse_data(fd_in, fd_out, blk_sz, hole_byte, start, end-start)) {
            return 3;
        }
    }

    return 0;
}

int do_map (int fd_in)
{
    off_t start  = lseek(fd_in, 0, SEEK_SET);
    if (start == -1) {
        perror("ERROR: input file is not seekable");
        return 1;
    }

    off_t end = 0;
    while (1) {
        start = lseek(fd_in, start, SEEK_DATA);
        if (start == -1) {
            if (errno == ENXIO) {
                 //EOF reached
                 break; 
            }
            perror("ERROR: could not seek");
            return 1;
        }
    
        end = lseek(fd_in, start, SEEK_HOLE);

        if (end == -1) {
            if (errno == ENXIO) {
                 //EOF reached
                 break; 
            }
            perror("ERROR: could not seek");
            return 1;
        }

        printf("%ld %ld\n", start, end - start);
        start = end;
    }

    if (start != -1 && end != -1)
        printf("%ld %ld\n", start, end - start);
    return 0;
}

int usage(const char* name)
{
    fprintf(stderr, "Helper utility to encode/decode sparse files to/from ursparse format\n\n");
    fprintf(stderr, "USAGE: %s options < input_file [ > output_file ]\n", name);
    fprintf(stderr, "       -h,    --help      shows usage\n");
    fprintf(stderr, "       -m,    --map       shows map of data blocks for sparse input file\n");
    fprintf(stderr, "       -u,    --ursparse  reads ursparse input file and writes sparse file to output (default option)\n");
    fprintf(stderr, "       -s,    --sparse    reads sparse input file and writes ursparse format to output file\n");
    //fprintf(stderr, "       -s00               reads sparse input file and writes ursparse format to output file\n");
    //fprintf(stderr, "                          all data blocks full of 0x00 will be treated as a hole\n");
    //fprintf(stderr, "       -sFF               reads sparse input file and writes ursparse format to output file\n");
    //fprintf(stderr, "                          all data blocks full of 0xFF will be treated as a hole\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "       -bSIZE,--blocksize=SIZE block size in bytes (defaults to 4096)\n");

    return 0;
}

enum actions {
    NONE = 0,
    USAGE,
    MAP,
    URSPARSE,
    SPARSE,
    SPARSE_XX
};


int byte_from_char(char a)
{
    if (a >= '0' && a <= '9') return a - '0';
    if (a >= 'a' && a <= 'f') return a - 'a';
    if (a >= 'A' && a <= 'F') return a - 'F';
    return -1;
}

int byte_from_hex(char a, char b, unsigned char* byte)
{
    int xa = byte_from_char(a);
    int xb = byte_from_char(b);
    if (xa == -1 || xb == -1) return -1;

    *byte = (xa & 0x0F) << 4 | (xb & 0x0F);
    return 0;
}

int main(int argc, const char* argv[]) 
{
    enum actions action = URSPARSE;
    unsigned char hole_byte = 0;

    int block_size = 4096;

    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') {
            if (argv[i][1] == '-') {
                if (!strcmp("help",     argv[i]+2)) { action = USAGE; break; }
                if (!strcmp("map",      argv[i]+2)) { action = MAP; break; }
                if (!strcmp("ursparse", argv[i]+2)) { action = URSPARSE; break; }
                if (!strcmp("sparse",   argv[i]+2)) { action = SPARSE; break; }
                if (!strcmp("blocksize=",  argv[i]+2)) { 
                    block_size = atoi(argv[i]+sizeof("blocksize="));
                    break; 
                }
            } 
            else {
                if (argv[i][1] == 'h' && argv[i][2] == 0) { action = USAGE; break; }
                if (argv[i][1] == 'm' && argv[i][2] == 0) { action = MAP; break; }
                if (argv[i][1] == 'u' && argv[i][2] == 0) { action = URSPARSE; break; }
                if (argv[i][1] == 's' && argv[i][2] == 0) { action = SPARSE; break; }
                if (argv[i][1] == 's' && argv[i][4] == 0) { 
                    if (byte_from_hex(argv[i][2], argv[i][3], &hole_byte)) {
                        usage(argv[0]);
                        return 2;
                    }
                    action=SPARSE_XX; break; 
                }
                if (argv[i][1] == 'b') { 
                    block_size = atoi(argv[i]+1);
                    break; 
                }
            }
        }
    }
    
    if (block_size < 2) {
        fprintf(stderr, "ERROR: invalid block size\n"); 
        return 3;
    }

    switch (action) {
    case USAGE:
        return usage(argv[0]);

    case MAP:
        return do_map(0);

    case URSPARSE:
        return do_ursparse(0, 1, block_size);

    case SPARSE:
        return do_sparse(0, 1, block_size, 0);

    case SPARSE_XX:
        return do_sparse(0, 1, block_size, &hole_byte);

    default:
        usage(argv[0]);
        return 1;
    }

    return 0;
}

