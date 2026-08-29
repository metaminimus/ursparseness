# ursparseness
utility to convert sparse files to and from ursparseness file format

#known issues
Round-trip silently loses trailing holes / final file size.
ursparseness emits nothing for a hole running to EOF, never sets the output's final size. 
A sparse file ending in a hole is reconstructed shorter without the last hole.
With -sHH this now also applies to a trailing all HH block. 
