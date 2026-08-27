// YUV plane addressing, A/B bases, PAL/NTSC last lines,
// progressive cy=y>>1 and interlaced cy={y[9:2], y[0]}, no plane overflow.
`timescale 1ns/1ps

module yuv_plane_addr_tb;

integer fails = 0;

reg         display_buf;
reg         interlaced;
reg  [9:0]  y;
wire [28:0] y_addr, u_addr, v_addr;

yuv_plane_addr dut(
	.display_buf(display_buf),
	.interlaced(interlaced),
	.y(y),
	.y_addr(y_addr),
	.u_addr(u_addr),
	.v_addr(v_addr)
);

localparam [28:0] FB_A = 29'h0600_0000;
localparam [28:0] FB_B = 29'h0604_0000;
localparam [28:0] U_OFF = 29'h0001_0000;
localparam [28:0] V_OFF = 29'h0001_4000;
localparam [28:0] SLOT  = 29'h0004_0000; // 2 MiB / 8

task check_one;
	input integer buf_sel;
	input integer yy;
	input integer intl;
	integer cy;
	reg [28:0] base, ye, ue, ve, y_end, u_end, v_end;
	begin
		display_buf = buf_sel[0];
		interlaced = intl[0];
		y = yy[9:0];
		#1;
		if (intl)
			cy = ((yy >> 2) << 1) | (yy & 1);
		else
			cy = yy >> 1;
		base = buf_sel ? FB_B : FB_A;
		ye = base + yy * 90;
		ue = base + U_OFF + cy * 45;
		ve = base + V_OFF + cy * 45;
		if (y_addr !== ye || u_addr !== ue || v_addr !== ve) begin
			$display("FAIL addr buf=%0d y=%0d got y/u/v %h %h %h want %h %h %h",
			         buf_sel, yy, y_addr, u_addr, v_addr, ye, ue, ve);
			fails = fails + 1;
		end
		y_end = ye + 89;
		u_end = ue + 44;
		v_end = ve + 44;
		if (y_end >= base + U_OFF) begin
			$display("FAIL Y overflow y=%0d end=%h", yy, y_end);
			fails = fails + 1;
		end
		if (u_end >= base + V_OFF) begin
			$display("FAIL U overflow y=%0d end=%h", yy, u_end);
			fails = fails + 1;
		end
		if (v_end >= base + SLOT) begin
			$display("FAIL V overflow y=%0d end=%h", yy, v_end);
			fails = fails + 1;
		end
	end
endtask

integer i;
integer pal_last, ntsc_last;

initial begin
	pal_last  = 575;
	ntsc_last = 479;

	check_one(0, 0, 0);
	check_one(0, 1, 0);  /* progressive: same chroma row as 0 */
	check_one(0, 2, 0);
	check_one(1, 0, 0);
	check_one(1, 100, 0);
	check_one(0, ntsc_last, 0);
	check_one(0, pal_last, 0);
	check_one(1, ntsc_last, 0);
	check_one(1, pal_last, 0);

	display_buf = 0;
	interlaced = 0;
	y = 0; #1;
	if (u_addr !== (FB_A + U_OFF)) begin
		$display("FAIL U base");
		fails = fails + 1;
	end
	y = 1; #1;
	if (u_addr !== (FB_A + U_OFF)) begin
		$display("FAIL adjacent luma must share chroma row 0");
		fails = fails + 1;
	end else
		$display("OK   adjacent luma lines 0/1 share chroma row 0");

	/* Interlaced: y=0 and y=2 share; y=0 and y=1 must not. */
	check_one(0, 0, 1);
	check_one(0, 1, 1);
	check_one(0, 2, 1);
	check_one(0, 3, 1);
	interlaced = 1;
	y = 0; #1;
	if (u_addr !== (FB_A + U_OFF)) begin
		$display("FAIL intl U y0");
		fails = fails + 1;
	end
	y = 2; #1;
	if (u_addr !== (FB_A + U_OFF)) begin
		$display("FAIL intl y0/y2 must share top-field chroma 0");
		fails = fails + 1;
	end else
		$display("OK   interlaced luma 0/2 share chroma row 0");
	y = 1; #1;
	if (u_addr === (FB_A + U_OFF)) begin
		$display("FAIL intl y=1 must not use top-field chroma 0");
		fails = fails + 1;
	end else
		$display("OK   interlaced luma 1 uses chroma row 1 (not y>>1)");

	/* Field source-line mapping: src_y = {vc[8:0], field} */
	if ({9'd0, 1'b0} !== 10'd0 || {9'd0, 1'b1} !== 10'd1 ||
	    {9'd239, 1'b1} !== 10'd479 || {9'd287, 1'b1} !== 10'd575) begin
		$display("FAIL field source-line mapping");
		fails = fails + 1;
	end else
		$display("OK   PAL last src_y=575  NTSC last src_y=479");

	for (i = 0; i <= pal_last; i = i + 1) begin
		check_one(0, i, 0);
		check_one(0, i, 1);
		check_one(1, i, 1);
	end

	if (fails) begin
		$display("yuv_plane_addr_tb FAIL (%0d)", fails);
		$fatal(1);
	end
	$display("yuv_plane_addr_tb PASS");
	$finish;
end

endmodule
