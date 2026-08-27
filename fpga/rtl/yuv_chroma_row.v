// MPEG-2 4:2:0 chroma *row* inside a frame-stored YUV420P plane.
//
// Progressive / frame (interlaced=0), FFmpeg YUV420P packing:
//   chroma_row = y >> 1
//   luma 0,1 share chroma 0; luma 2,3 share chroma 1.
//
// Interlaced MPEG-2 4:2:0 (interlaced=1), field-based chroma in the same
// height/2 U/V planes (FFmpeg mpeg2 frame store):
//   chroma_row = {y[9:2], y[0]}   // (y>>2)*2 + (y&1)
//   luma 0,2 (top field)  share chroma 0
//   luma 1,3 (bottom field) share chroma 1
//   luma 4,6 share chroma 2, …
//
// Spatial field membership is even/odd *frame* line, not TFF/BFF.
// TFF/BFF is temporal display order and must not swap this row map.
// Never interpolate chroma between rows 2i and 2i+1 on interlaced material.
//
// Horizontal LEFT siting is not this module: chroma_col = x >> 1.

module yuv_chroma_row
(
	input         interlaced,
	input  [9:0]  y,
	output [9:0]  cy
);

wire [9:0] cy_prog = {1'b0, y[9:1]};
wire [9:0] cy_intl = {y[9:2], y[0]};

assign cy = interlaced ? cy_intl : cy_prog;

endmodule
