#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64   //64-bit off_t / lseek / pread even on 32-bit builds
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

//largest value representable in off_t (a signed type in POSIX): all-ones in
//the widest unsigned type, shifted right one, then narrowed to off_t
#define OFF_MAX ((off_t)((~(uintmax_t)0) >> 1))

//EINTR-safe wrappers: a signal must not abort a transfer mid-stream
static ssize_t read_eintr(int fd, void* buf, size_t n)
{
    ssize_t r;
    do { r = read(fd, buf, n); } while (r == -1 && errno == EINTR);
    return r;
}

static ssize_t write_eintr(int fd, const void* buf, size_t n)
{
    ssize_t r;
    do { r = write(fd, buf, n); } while (r == -1 && errno == EINTR);
    return r;
}

static ssize_t pread_eintr(int fd, void* buf, size_t n, off_t off)
{
    ssize_t r;
    do { r = pread(fd, buf, n, off); } while (r == -1 && errno == EINTR);
    return r;
}

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

//running state for parse_uint, held by the caller across calls: a single
//number may be split over several input buffers, so the accumulated value
//and "have we seen a numeral yet" must survive between calls. one instance
//per number being parsed.
struct parse_uint_state_data {
    off_t value;    //parsed value, and running accumulator between calls
    int   started;  //set once the first numeral has been consumed
};

struct ursparse {
    struct parse_uint_state_data offset;
    struct parse_uint_state_data size;
};

//parses unsigned integer
//ignores spaces before first numeral
//
//buff, sz   => input buffer to parse and its size
//
//out_sz     => output number of bytes consumed
//data       => in/out parse state (value + started flag); must be the same
//              instance for every call parsing one number. tracking "started"
//              separately from the value matters because a value of 0 split
//              at a buffer boundary must not be mistaken for "not started
//              yet" (which would skip and eat the following separator)
//what       => noun for diagnostics ("offset", "length", "block size", ...)
//
//returns
//  negative number on error
//  0 done parsing, data->value contains the parsed value
//  n intermediate parsing,
//    need more data,
//    data->value contains intermediate value,
int parse_uint(const char* buff, size_t sz, size_t* out_sz, struct parse_uint_state_data* data, const char* what)
{
    if (!buff) return -1;
    if (!sz) return -2;
    if (!out_sz) return -3;
    if (!data) return -4;

    size_t i = 0;

    if (!data->started) {
        //consume spaces and newlines before number
        for(; i < sz; ++i)
            if (!(buff[i] == ' ' || buff[i] == '\n' )) break;
    }

    for (; i < sz; ++i) {
        if (buff[i] >= '0' && buff[i] <= '9') {
            //numerals
            data->started = 1;
            int d = buff[i] - '0';
            if (data->value > (OFF_MAX - d) / 10) {
                fprintf(stderr, "ERROR: number too large parsing %s\n", what);
                return -1;
            }
            data->value = data->value * 10 + d;
            continue;
        }

        if (buff[i] == ' ' || buff[i] == '\n') {
            //spaces or newline after number 
            *out_sz = i;
            return 0; //done parsing
        }

        fprintf(stderr, "ERROR: invalid char parsing %s: %c\n", what, buff[i]);
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

        fprintf(stderr, "ERROR: invalid char parsing newline pos %zu: %c\n", i, buff[i]);
        return -1;
    }

    //reach EOB
    *out_sz = i;
    return i;
}
 
//
//writes the meat to output file
//
int do_meat(int fd_out, const char* buff, size_t sz, size_t* out_sz)
{
    if (!buff)   return -1;
    if (!sz)     return -2;
    if (!out_sz) return -3;

    size_t written_bytes = 0;

    while (sz > 0) {
        ssize_t r = write_eintr(fd_out, buff, sz);

        if (-1 == r) {
            perror("ERROR: could not write to output file");
            *out_sz = written_bytes;
            return -4;
        }

        if (0 == r) {
            //write() accepted nothing though sz > 0; retrying would spin forever
            fprintf(stderr, "ERROR: write to output file made no progress\n");
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
int do_hole(int fd_out, off_t offset)
{
    off_t r = lseek(fd_out, offset, SEEK_SET);
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
    //set once a digit of the current segment's header has been consumed;
    //cleared (with the rest of the struct) when a segment completes.
    //lets the caller tell "cut mid-segment" from "clean end + trailing space"
    int seg_started;
    //offset+length of the last completed segment; segments must not move
    //backwards into already-written data. survives the per-segment reset.
    off_t prev_end;
};

//clears the per-segment parse state for the next segment, keeping prev_end
static void reset_segment(struct ursparse_state_data* data)
{
    off_t prev_end = data->prev_end;
    memset(data, 0, sizeof(*data));
    data->prev_end = prev_end;
}

//parses ursparse line
//offset length \n
//
//buff, sz  => buffer to parse and its size
//out_sz    => output number of bytes consumed
//data      => in/out parser internal state
//fd_out    => output fd the holes and meat are written to
//max_size  => if >= 0, reject any segment whose offset+length would push the
//             reconstructed file past this many bytes (returns -9)
//
//returns
//  negative number on error
//  0 done parsing ursparse line
//  n intermediate parsing
//    need more data
int parse_ursparse(const char* buff, size_t sz, size_t* out_sz, struct ursparse_state_data* data, int fd_out, off_t max_size)
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
        __attribute__((fallthrough));

    case PARSE_OFFSET:

        r = parse_uint(buff, sz, out_sz, &data->ursparse.offset, "offset");
        if (r < 0) {
            fprintf(stderr, "ERROR: could not parse offset\n");
            data->state = PARSE_ERROR;
            return r;
        }

        //parse_uint only ever stops on a digit when it hits end-of-buffer,
        //so a trailing digit in the consumed range means the header started
        if (*out_sz > 0 && buff[*out_sz - 1] >= '0' && buff[*out_sz - 1] <= '9')
            data->seg_started = 1;

        buff += *out_sz;
        sz -= *out_sz;
        extra_sz += *out_sz;

        if (r > 0) {
            //report every byte consumed so far this call, not just this stage's,
            //so the caller advances its cursor past the already-parsed prefix
            *out_sz = extra_sz;
            return *out_sz;
        }

        data->state = PARSE_SIZE;
        __attribute__((fallthrough));

    case PARSE_SIZE:

        //data->ursparse.size has its own started flag, so parse_uint still
        //skips the space between the offset and the size
        r = parse_uint(buff, sz, out_sz, &data->ursparse.size, "length");

        if (r < 0) {
            fprintf(stderr, "ERROR: could not parse length\n");
            data->state = PARSE_ERROR;
            return r;
        }

        buff += *out_sz;
        sz -= *out_sz;
        extra_sz += *out_sz;

        if (r > 0) {
            //report every byte consumed so far this call, not just this stage's,
            //so the caller advances its cursor past the already-parsed prefix
            *out_sz = extra_sz;
            return *out_sz;
        }

        data->state = PARSE_NEWLINE;
        __attribute__((fallthrough));

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
            //report every byte consumed so far this call, not just this stage's,
            //so the caller advances its cursor past the already-parsed prefix
            *out_sz = extra_sz;
            return *out_sz;
        }

        {
        off_t off = data->ursparse.offset.value;
        off_t len = data->ursparse.size.value;

        //segments must march forward: a segment that starts inside data an
        //earlier segment already wrote would overwrite it (and turn an
        //intended hole into data)
        if (off < data->prev_end) {
            fprintf(stderr,
                "ERROR: segment offset %jd overlaps earlier data ending at %jd\n",
                (intmax_t)off, (intmax_t)data->prev_end);
            data->state = PARSE_ERROR;
            return -10;
        }

        //refuse to expand a stream whose segments would reach past the
        //requested ceiling. offset+size is the file's high-water mark;
        //the subtraction form avoids overflowing off_t
        if (max_size >= 0 && (len > max_size || off > max_size - len)) {
            fprintf(stderr,
                "ERROR: segment at offset %jd (length %jd) exceeds --max-size %jd\n",
                (intmax_t)off, (intmax_t)len, (intmax_t)max_size);
            data->state = PARSE_ERROR;
            return -9;
        }

        //new high-water mark, clamped so off+len can't overflow off_t
        data->prev_end = (len > OFF_MAX - off) ? OFF_MAX : off + len;

        fprintf(stderr, "INFO: processing segment %jd %jd\n",
                (intmax_t)off, (intmax_t)len);

        r = do_hole(fd_out, off);
        if (r < 0) {
            data->state = PARSE_ERROR;
            return -8;
        }
        }

        data->state = PARSE_MEAT;
        __attribute__((fallthrough));

    case PARSE_MEAT:

        if (!data->ursparse.size.value) {
            //zero-length segment: the hole (if any) is already seeked to,
            //there is no meat, so the segment is complete
            *out_sz = extra_sz;
            reset_segment(data);
            return 0;
        }

        if (sz == 0) {
            //the header consumed the rest of this buffer; the meat is still
            //to come, so report progress and wait for the next read
            *out_sz = extra_sz;
            return *out_sz;
        }

        {
        size_t want = (data->ursparse.size.value < (off_t)sz)
                    ? (size_t)data->ursparse.size.value : sz;

        r = do_meat(fd_out, buff, want, out_sz);
        }

        if (r < 0) {
            fprintf(stderr, "ERROR: could not process meat\n");
            data->state = PARSE_ERROR;
            return r;
        }

        if (data->ursparse.size.value < (off_t)*out_sz) {
            fprintf(stderr, "ERROR: meat processor broken\n");
            data->state = PARSE_ERROR;
            return -7;
        }

        data->ursparse.size.value -= *out_sz;
        *out_sz += extra_sz;

        if (!data->ursparse.size.value) {
            reset_segment(data);
            return 0;
        }

        return *out_sz;

    case PARSE_ERROR:
        return -5;

    default:
        return -6;
    }
}

int do_ursparse(int fd_in, int fd_out, size_t blk_sz, off_t max_size)
{
    //input is a sequential ursparse stream (pipe-friendly, no seek needed)
    //output must be seekable so holes can be recreated via lseek
    if (lseek(fd_out, 0, SEEK_CUR) == -1) {
        perror("ERROR: output file is not seekable");
        return 1;
    }

    //O_APPEND forces every write to EOF regardless of lseek, so do_hole's
    //seeks would be silently ignored and every segment concatenated
    int flags = fcntl(fd_out, F_GETFL);
    if (flags == -1) {
        perror("ERROR: could not query output file");
        return 1;
    }
    if (flags & O_APPEND) {
        fprintf(stderr, "ERROR: output file is in append mode; holes cannot be recreated (redirect with > not >>)\n");
        return 1;
    }

    char* read_buff = malloc(blk_sz);
    if (!read_buff) {
        fprintf(stderr, "ERROR: could not allocate memory: %zu bytes\n", blk_sz);
        return 2;
    }

    struct ursparse_state_data data;
    memset(&data, 0, sizeof(data));

    while (1) {
        ssize_t nbytes = read_eintr(fd_in, read_buff, blk_sz);

        if (nbytes == -1) {
            perror("ERROR: could not read from input file");
            free(read_buff);
            return 3;
        }

        if (!nbytes) {
            //EOF: a well-formed stream ends exactly on a segment boundary,
            //which resets the parser (seg_started back to 0). If a segment's
            //header or meat was in progress, the stream was cut mid-transfer.
            if (data.seg_started || data.state == PARSE_MEAT) {
                fprintf(stderr, "ERROR: input ends mid-segment, ursparse stream is truncated\n");
                free(read_buff);
                return 5;
            }
            break; //EOF reached
        }

        for (size_t cursor = 0; cursor < (size_t)nbytes; ) {

            size_t out_sz = 0;
            int r = parse_ursparse(read_buff + cursor, (size_t)nbytes - cursor, &out_sz, &data, fd_out, max_size);

            if (r < 0) {
                free(read_buff);
                //-9 => --max-size exceeded, -10 => segments out of order
                return r == -9 ? 6 : r == -10 ? 7 : 4;
            }

            cursor += out_sz;
        }
    }

    free(read_buff);
    return 0;
}

//
//copies sz bytes from fd_in (at offset start) to fd_out (at its current
//position), looping until all bytes are transferred.
//
//falls back to a read/write loop when copy_file_range is unavailable
//(old kernel, cross-filesystem, or not supported for these fds)
//
static char copy_rw_buff[65536];

int do_sparse_copy_rw(int fd_in, int fd_out, off_t start, size_t sz)
{
    while (sz > 0) {
        size_t want = sz < sizeof(copy_rw_buff) ? sz : sizeof(copy_rw_buff);

        ssize_t nr = pread_eintr(fd_in, copy_rw_buff, want, start);
        if (nr == -1) {
            perror("ERROR: could not read data");
            return -1;
        }
        if (nr == 0) {
            fprintf(stderr, "ERROR: short read copying data, %zu bytes missing\n", sz);
            return -1;
        }

        for (ssize_t off = 0; off < nr; ) {
            ssize_t nw = write_eintr(fd_out, copy_rw_buff + off, nr - off);
            if (nw == -1) {
                perror("ERROR: could not copy data");
                return -1;
            }
            if (nw == 0) {
                //write() accepted nothing; retrying would spin forever
                fprintf(stderr, "ERROR: write to output made no progress\n");
                return -1;
            }
            off += nw;
        }

        start += nr;
        sz -= nr;
    }

    return 0;
}

int do_sparse_copy_data(int fd_in, int fd_out, off_t start, size_t sz)
{
    while (sz > 0) {
        ssize_t r = copy_file_range(fd_in, &start, fd_out, 0, sz, 0);

        if (r == -1) {
            if (errno == EINTR) {
                //interrupted before copying anything; retry
                continue;
            }
            if (errno == ENOSYS || errno == EXDEV || errno == EINVAL
                    || errno == EOPNOTSUPP) {
                //copy_file_range not usable here; fall back
                return do_sparse_copy_rw(fd_in, fd_out, start, sz);
            }
            perror("ERROR: could not copy data");
            return -1;
        }

        if (r == 0) {
            fprintf(stderr, "ERROR: short copy of data, %zu bytes missing\n", sz);
            return -1;
        }

        sz -= r;
    }

    return 0;
}

//emits one contiguous segment: the "offset length\n" header then the bytes
int do_sparse_data(int fd_in, int fd_out, off_t start, size_t sz)
{
    fprintf(stderr, "INFO: processing segment %jd %zu\n", (intmax_t)start, sz);

    if (4 > dprintf(fd_out, "%jd %zu\n", (intmax_t)start, sz)) {
        perror("ERROR: could not write segment");
        return -1;
    }
    if (-1 == do_sparse_copy_data(fd_in, fd_out, start, sz)) {
        return -1;
    }
    return 0;
}

//reads exactly n bytes from fd at offset off into buf
static int read_full(int fd, char* buf, size_t n, off_t off)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread_eintr(fd, buf + got, n - got, off + (off_t)got);
        if (r == -1) {
            perror("ERROR: could not read data");
            return -1;
        }
        if (r == 0) {
            fprintf(stderr, "ERROR: short read scanning data, %zu bytes missing\n", n - got);
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

//like do_sparse_data, but for the -sHH modes: walks [start, start+sz) in
//blk_sz blocks and drops every block that is entirely `hole_byte`, emitting
//each surviving run of blocks as its own segment. buf must hold blk_sz bytes.
int do_sparse_data_holy(int fd_in, int fd_out, char* buf, size_t blk_sz,
                        unsigned char hole_byte, off_t start, size_t sz)
{
    off_t pos = start;
    size_t left = sz;
    off_t run_start = -1;   //-1 => no run open

    while (left > 0) {
        size_t chunk = left < blk_sz ? left : blk_sz;

        if (read_full(fd_in, buf, chunk, pos))
            return -1;

        int is_hole = 1;
        for (size_t k = 0; k < chunk; ++k)
            if ((unsigned char)buf[k] != hole_byte) { is_hole = 0; break; }

        if (is_hole) {
            if (run_start != -1) {
                if (do_sparse_data(fd_in, fd_out, run_start, (size_t)(pos - run_start)))
                    return -1;
                run_start = -1;
            }
        }
        else if (run_start == -1) {
            run_start = pos;
        }

        pos  += (off_t)chunk;
        left -= chunk;
    }

    if (run_start != -1)
        return do_sparse_data(fd_in, fd_out, run_start, (size_t)(pos - run_start));

    return 0;
}

int do_sparse(int fd_in, int fd_out, size_t blk_sz, unsigned char* hole_byte)
{
    char* scan_buf = 0;
    int ret = 0;

    if (hole_byte) {
        //-sHH needs a block-sized buffer to test each block for the hole byte
        scan_buf = malloc(blk_sz);
        if (!scan_buf) {
            fprintf(stderr, "ERROR: could not allocate memory: %zu bytes\n", blk_sz);
            return 4;
        }
    }

    off_t start = lseek(fd_in, 0, SEEK_SET);
    if (start == -1) {
        perror("ERROR: input file is not seekable");
        ret = 1;
        goto done;
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
            ret = 1;
            goto done;
        }

        end = lseek(fd_in, start, SEEK_HOLE);

        if (end == -1) {
            if (errno == ENXIO) {
                 //EOF reached
                 break;
            }
            perror("ERROR: could not seek");
            ret = 1;
            goto done;
        }

        if (end <= start) {
            //no forward progress: the input is not a regular file with real
            //data extents (e.g. /dev/null, where every lseek returns 0).
            //stop rather than spin forever.
            break;
        }

        int rc = hole_byte
            ? do_sparse_data_holy(fd_in, fd_out, scan_buf, blk_sz, *hole_byte, start, end-start)
            : do_sparse_data(fd_in, fd_out, start, end-start);
        if (rc) {
            ret = 2;
            goto done;
        }
        start = end;
    }

done:
    free(scan_buf);
    return ret;
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

        if (end <= start) {
            //no forward progress (e.g. /dev/null); stop rather than spin forever
            break;
        }

        if (printf("%jd %jd\n", (intmax_t)start, (intmax_t)(end - start)) < 0) {
            perror("ERROR: could not write map");
            return 1;
        }
        start = end;
    }

    //printf only buffers; a write error to a pipe/full disk surfaces on flush
    if (fflush(stdout) != 0) {
        perror("ERROR: could not write map");
        return 1;
    }

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
    fprintf(stderr, "       -sHH              like -s, but also treats any data block that is entirely\n");
    fprintf(stderr, "                         byte 0xHH as a hole\n");
    fprintf(stderr, "       -s00              treat data blocks that are all 0x00 as holes\n");
    fprintf(stderr, "       -sFF              treat data blocks that are all 0xFF as holes\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "       -bSIZE,--blocksize=SIZE block size in bytes (defaults to 4096)\n");
    fprintf(stderr, "       -MSIZE,--max-size=SIZE  abort -u expansion if meat+holes would exceed SIZE bytes\n");

    return 0;
}

enum actions {
    USAGE,
    MAP,
    URSPARSE,
    SPARSE,
    SPARSE_XX,
    ERROR
};


int byte_from_char(char a)
{
    if (a >= '0' && a <= '9') return a - '0';
    if (a >= 'a' && a <= 'f') return a - 'a' + 10;
    if (a >= 'A' && a <= 'F') return a - 'A' + 10;
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

//parses a block size argument
//returns the value on success, or -1 on overflow / trailing garbage / out of range
int parse_block_size(const char* s)
{
    struct parse_uint_state_data pu = { 0, 0 };
    size_t consumed = 0;
    size_t len = strlen(s);

    //reuse the stream integer parser; it stops at a space/newline (returns 0)
    //or at end-of-buffer (returns >0), so a valid argument is one where the
    //whole string was consumed as digits
    int r = parse_uint(s, len, &consumed, &pu, "block size");
    if (r < 0 || consumed != len || pu.value < 2 || pu.value > (1 << 30))
        return -1;

    return (int)pu.value;
}

//parses a byte-count argument (0 .. OFF_MAX)
//returns the value on success, or -1 on trailing garbage / overflow / empty
off_t parse_size(const char* s)
{
    struct parse_uint_state_data pu = { 0, 0 };
    size_t consumed = 0;
    size_t len = strlen(s);

    int r = parse_uint(s, len, &consumed, &pu, "max size");
    if (r < 0 || consumed != len)
        return -1;

    return pu.value;
}

int main(int argc, const char* argv[])
{
    enum actions action = URSPARSE;
    unsigned char hole_byte = 0;

    int block_size = 4096;
    off_t max_size = -1;   //-1 => no limit

    //track which tuning options were actually supplied, so we can reject
    //combinations the chosen action would silently ignore
    int block_size_given = 0;
    int max_size_given   = 0;
    int hole_byte_given  = 0;

    //a broken output pipe or an exceeded file-size limit should surface as a
    //write() error we can report, not a signal that kills us mid-transfer
    signal(SIGPIPE, SIG_IGN);
    signal(SIGXFSZ, SIG_IGN);

    //every option is processed; when an option is repeated the last one wins.
    //an unrecognised or malformed argument is a hard error rather than being
    //silently ignored.
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];

        if (arg[0] != '-' || arg[1] == 0) {
            fprintf(stderr, "ERROR: unexpected argument: %s\n", arg);
            usage(argv[0]);
            return 1;
        }

        if (arg[1] == '-') {
            const char* opt = arg + 2;
            if      (!strcmp("help",     opt)) action = USAGE;
            else if (!strcmp("map",      opt)) action = MAP;
            else if (!strcmp("ursparse", opt)) action = URSPARSE;
            else if (!strcmp("sparse",   opt)) action = SPARSE;
            else if (!strncmp("blocksize=", opt, sizeof("blocksize=")-1)) {
                block_size = parse_block_size(opt + sizeof("blocksize=")-1);
                block_size_given = 1;
            }
            else if (!strncmp("max-size=", opt, sizeof("max-size=")-1)) {
                max_size = parse_size(opt + sizeof("max-size=")-1);
                max_size_given = 1;
                if (max_size < 0) {
                    fprintf(stderr, "ERROR: invalid max size: %s\n", arg);
                    usage(argv[0]);
                    return 3;
                }
            }
            else {
                fprintf(stderr, "ERROR: unknown option: %s\n", arg);
                usage(argv[0]);
                return 1;
            }
        }
        else {
            //short-circuit &&: arg[3]/arg[4] are only read once the earlier
            //bytes are known non-NUL, so this never reads past the string
            if      (!strcmp("h", arg+1)) action = USAGE;
            else if (!strcmp("m", arg+1)) action = MAP;
            else if (!strcmp("u", arg+1)) action = URSPARSE;
            else if (!strcmp("s", arg+1)) action = SPARSE;
            else if (arg[1] == 's' && arg[2] && arg[3] && !arg[4]) {
                if (byte_from_hex(arg[2], arg[3], &hole_byte)) {
                    fprintf(stderr, "ERROR: invalid hole byte: %s\n", arg);
                    usage(argv[0]);
                    return 2;
                }
                action = SPARSE_XX;
                hole_byte_given = 1;
            }
            else if (arg[1] == 'b') {
                block_size = parse_block_size(arg + 2);
                block_size_given = 1;
            }
            else if (arg[1] == 'M') {
                max_size = parse_size(arg + 2);
                max_size_given = 1;
                if (max_size < 0) {
                    fprintf(stderr, "ERROR: invalid max size: %s\n", arg);
                    usage(argv[0]);
                    return 3;
                }
            }
            else {
                fprintf(stderr, "ERROR: unknown option: %s\n", arg);
                usage(argv[0]);
                return 1;
            }
        }
    }

    if (argc < 2) {
        action = ERROR;
    }

    //--help wins over everything and never complains about companion flags
    if (action == USAGE) {
        return usage(argv[0]);
    }

    //reject tuning options the selected action would silently ignore
    if (max_size_given && action != URSPARSE) {
        fprintf(stderr, "ERROR: --max-size/-M only applies to -u (stream expansion)\n");
        return 3;
    }
    if (hole_byte_given && action != SPARSE_XX) {
        fprintf(stderr, "ERROR: a hole byte (-sHH) cannot be combined with another mode\n");
        return 3;
    }
    if (block_size_given && action == MAP) {
        fprintf(stderr, "ERROR: -b/--blocksize has no effect with -m\n");
        return 3;
    }

    if (block_size < 2) {
        fprintf(stderr, "ERROR: invalid block size\n");
        return 3;
    }

    switch (action) {
    case MAP:
        return do_map(0);

    case URSPARSE:
        return do_ursparse(0, 1, block_size, max_size);

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

