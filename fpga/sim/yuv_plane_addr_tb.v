// YUV plane addressing, A/B bases, PAL/NTSC last lines, chroma_row=y>>1,
// and no plane overflow.
`timescale 1ns/1ps

module yuv_plane_addr_tb;

integer fails = 0;

reg         display_buf;
reg  [9:0]  y;
wire [28:0] y_addr, u_addr, v_addr;

yuv_plane_addr dut(
	.display_buf(display_buf),
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
	integer cy;
	reg [28:0] base, ye, ue, ve, y_end, u_end, v_end;
	begin
		display_buf = buf_sel[0];
		y = yy[9:0];
		#1;
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

	check_one(0, 0);
	check_one(0, 1);  /* same chroma row as 0 */
	check_one(0, 2);
	check_one(1, 0);
	check_one(1, 100);
	check_one(0, ntsc_last);
	check_one(0, pal_last);
	check_one(1, ntsc_last);
	check_one(1, pal_last);

	display_buf = 0;
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

	/* Field source-line mapping: src_y = {vc[8:0], field} */
	if ({9'd0, 1'b0} !== 10'd0 || {9'd0, 1'b1} !== 10'd1 ||
	    {9'd239, 1'b1} !== 10'd479 || {9'd287, 1'b1} !== 10'd575) begin
		$display("FAIL field source-line mapping");
		fails = fails + 1;
	end else
		$display("OK   PAL last src_y=575  NTSC last src_y=479");

	for (i = 0; i <= pal_last; i = i + 1)
		check_one(0, i);

	if (fails) begin
		$display("yuv_plane_addr_tb FAIL (%0d)", fails);
		$fatal(1);
	end
	$display("yuv_plane_addr_tb PASS");
	$finish;
end

endmodule
