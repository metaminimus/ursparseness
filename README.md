# ursparseness
utility to convert sparse files to and from ursparseness file format

#trailing holes
A hole running to EOF has no data extent, so nothing in the segment stream
would otherwise record where the file ends and the decoder would rebuild it
short. -s closes the stream with a zero-length segment at the final size
(`262144 0\n`), which carries the size without carrying any bytes, and -u
sizes the output to the last segment's end. -sHH does the same for a trailing
run of all-HH blocks.

The segment is emitted only when the file really does end in a hole, so a file
ending in data encodes exactly as before, and streams written by older
versions still decode.

Output must be a regular file for the final size to be set; a block device is
already a fixed size and is left alone.
