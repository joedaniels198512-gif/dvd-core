// Combinational chroma-row mapper vs the C golden model.
`timescale 1ns/1ps

module yuv_chroma_row_tb;

integer fails = 0;

reg         interlaced;
reg  [9:0]  y;
wire [9:0]  cy;

yuv_chroma_row dut(
	.interlaced(interlaced),
	.y(y),
	.cy(cy)
);

function integer ref_cy;
	input integer intl;
	input integer yy;
	begin
		if (intl)
			ref_cy = ((yy >> 2) << 1) | (yy & 1);
		else
			ref_cy = yy >> 1;
	end
endfunction

task check;
	input integer intl;
	input integer yy;
	integer want;
	begin
		interlaced = intl[0];
		y = yy[9:0];
		#1;
		want = ref_cy(intl, yy);
		if (cy !== want[9:0]) begin
			$display("FAIL intl=%0d y=%0d got=%0d want=%0d",
			         intl, yy, cy, want);
			fails = fails + 1;
		end
	end
endtask

integer i;

initial begin
	for (i = 0; i <= 575; i = i + 1) begin
		check(0, i);
		check(1, i);
	end

	/* Progressive: adjacent frame lines share. */
	check(0, 0);
	check(0, 1);
	interlaced = 0; y = 0; #1;
	if (cy !== 10'd0) begin $display("FAIL prog y0"); fails = fails + 1; end
	y = 1; #1;
	if (cy !== 10'd0) begin $display("FAIL prog y1 share"); fails = fails + 1; end
	y = 2; #1;
	if (cy !== 10'd1) begin $display("FAIL prog y2"); fails = fails + 1; end

	/* Interlaced: same field shares; opposite fields differ. */
	interlaced = 1;
	y = 0; #1; if (cy !== 10'd0) begin $display("FAIL intl y0"); fails = fails + 1; end
	y = 2; #1; if (cy !== 10'd0) begin $display("FAIL intl y2 share top"); fails = fails + 1; end
	y = 1; #1; if (cy !== 10'd1) begin $display("FAIL intl y1"); fails = fails + 1; end
	y = 3; #1; if (cy !== 10'd1) begin $display("FAIL intl y3 share bot"); fails = fails + 1; end
	y = 4; #1; if (cy !== 10'd2) begin $display("FAIL intl y4"); fails = fails + 1; end

	/* CRT field 0 (even src_y) never selects odd chroma rows. */
	for (i = 0; i < 288; i = i + 1) begin
		interlaced = 1;
		y = {i[8:0], 1'b0};
		#1;
		if (cy[0] !== 1'b0) begin
			$display("FAIL field0 chroma odd y=%0d cy=%0d", y, cy);
			fails = fails + 1;
		end
	end
	for (i = 0; i < 288; i = i + 1) begin
		interlaced = 1;
		y = {i[8:0], 1'b1};
		#1;
		if (cy[0] !== 1'b1) begin
			$display("FAIL field1 chroma even y=%0d cy=%0d", y, cy);
			fails = fails + 1;
		end
	end

	/* PAL / NTSC last lines. */
	check(0, 479);
	check(0, 575);
	check(1, 479);
	check(1, 575);

	if (fails) begin
		$display("yuv_chroma_row_tb FAIL (%0d)", fails);
		$fatal(1);
	end
	$display("yuv_chroma_row_tb PASS");
	$finish;
end

endmodule
