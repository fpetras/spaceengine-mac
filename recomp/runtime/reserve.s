// Sets aside address space for the Windows image inside this program's own
// Mach-O image, so nothing else can be placed there before it is mapped.
//
// The image needs 0x140000000..0x141a93000. The Makefile puts this segment at
// 0x130000000 before the load-time slide; with 288 MB it still covers the
// image range for any slide up to 256 MB (observed slides stay under 80 MB).
// Zero-fill: it takes no space in the file and no memory until touched.
.globl _recomp_reserved_range
.zerofill __SEIMAGE,__reserve,_recomp_reserved_range,0x12000000,14
