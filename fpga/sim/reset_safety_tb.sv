// Reset-safety regression for the DDRAM users in DVD.sv.
//
// Reproduces the proven MiSTer main= lifecycle failure: the second
// app_restart() asserts fpga_core_reset(1) while the core is running.
// sysmem.sv routes the same reset (2-FF synchronized) into
// f2sdram_safe_terminator, which disconnects the core's DDRAM commands
// from the bridge while reset is held. Pre-fix RTL kept the mailbox FSM
// polling during reset: its read was silently dropped and ST_RD_MB_W hung
// forever (set_seq frozen, video fills dead, black screen).
//
// This bench instantiates the real emu (DVD.sv) and the real framework
// f2sdram_safe_terminator with a behavioural Avalon f2sdram model, then
// pulses core reset at pseudo-random points mid-activity and requires,
// after every release:
//   - set_seq resumes incrementing (mailbox polling recovered)
//   - JOY publishes resume (mailbox writes recovered)
//   - video line-fill read bursts resume (arbitration recovered)
// With -DSTRICT_AVALON it also requires that no command is ever withdrawn
// or mutated while waitrequest is high (Avalon hold rule).
`timescale 1ns/1ps

// ---------------------------------------------------------------- stubs ---
module pll
(
	input  refclk,
	input  rst,
	output outclk_0
);
assign outclk_0 = refclk;
endmodule

module hps_io #(parameter CONF_STR = "", parameter CONF_STR_BRAM = 0)
(
	input                clk_sys,
	inout        [45:0]  HPS_BUS,
	inout        [35:0]  EXT_BUS,
	inout        [21:0]  gamma_bus,
	output               forced_scandoubler,
	output        [1:0]  buttons,
	output      [127:0]  status,
	input        [15:0]  status_menumask,
	output       [31:0]  joystick_0,
	output       [10:0]  ps2_key
);
assign forced_scandoubler = 1'b0;
assign buttons    = 2'b00;
assign status     = 128'd0;
assign joystick_0 = 32'd0;
assign ps2_key    = 11'd0;
endmodule

// ------------------------------------------------------------------ top ---
module reset_safety_tb;

integer fails  = 0;
integer seed   = 32'h5EED_0001;
integer n_viol = 0;

function integer rnd;
	input integer dummy;
	integer v;
	begin
		v = $random(seed);
		rnd = (v < 0) ? -v : v;
	end
endfunction

reg clk = 0;
always #18.5 clk = ~clk;   // ~27 MHz

// Models sys_top reset_out (Main's fpga_core_reset via gpo).
reg reset_req = 1;

// DUT <-> terminator slave side
wire        s_wait;
wire  [7:0] s_cnt;
wire [28:0] s_addr;
wire [63:0] s_rdata;
wire        s_rvalid;
wire        s_read;
wire [63:0] s_wdata;
wire  [7:0] s_be;
wire        s_write;

// terminator master side <-> bridge model
wire        m_wait;
wire  [7:0] m_cnt;
wire [28:0] m_addr;
reg  [63:0] m_rdata = 64'd0;
reg         m_rvalid = 0;
wire        m_read;
wire [63:0] m_wdata;
wire  [7:0] m_be;
wire        m_write;

// Terminator reset sync, exactly as sysmem.sv ram1_reset_0/1.
reg t_rst0 = 1, t_rst1 = 1;
always @(posedge clk) begin
	t_rst0 <= reset_req;
	t_rst1 <= t_rst0;
end

f2sdram_safe_terminator #(64, 8) term
(
	.clk(clk),
	.rst_req_sync(t_rst1),

	.waitrequest_master(m_wait),
	.burstcount_master(m_cnt),
	.address_master(m_addr),
	.readdata_master(m_rdata),
	.readdatavalid_master(m_rvalid),
	.read_master(m_read),
	.writedata_master(m_wdata),
	.byteenable_master(m_be),
	.write_master(m_write),

	.waitrequest_slave(s_wait),
	.burstcount_slave(s_cnt),
	.address_slave(s_addr),
	.readdata_slave(s_rdata),
	.readdatavalid_slave(s_rvalid),
	.read_slave(s_read),
	.writedata_slave(s_wdata),
	.byteenable_slave(s_be),
	.write_slave(s_write)
);

emu dut
(
	.CLK_50M(clk),
	.RESET(reset_req),
	.HPS_BUS(),
	.HDMI_WIDTH(12'd0),
	.HDMI_HEIGHT(12'd0),
	.CLK_AUDIO(clk),
	.SD_MISO(1'b0),
	.SD_CD(1'b0),
	.UART_CTS(1'b0),
	.UART_RXD(1'b0),
	.UART_DSR(1'b0),
	.USER_IN(7'h7f),
	.OSD_STATUS(1'b0),

	.DDRAM_CLK(),
	.DDRAM_BUSY(s_wait),
	.DDRAM_BURSTCNT(s_cnt),
	.DDRAM_ADDR(s_addr),
	.DDRAM_DOUT(s_rdata),
	.DDRAM_DOUT_READY(s_rvalid),
	.DDRAM_RD(s_read),
	.DDRAM_DIN(s_wdata),
	.DDRAM_BE(s_be),
	.DDRAM_WE(s_write)
);

// --------------------------------------------- Avalon f2sdram bridge model
localparam [28:0] MB_ADDR  = 29'h0608_0000;
localparam [28:0] JOY_ADDR = 29'h0608_0001;
localparam [28:0] SET_ADDR = 29'h0608_0002;
localparam [28:0] CTL_ADDR = 29'h0608_0003;

localparam QD = 8;
reg [28:0] q_addr [0:QD-1];
reg  [7:0] q_cnt  [0:QD-1];
integer    q_head = 0, q_tail = 0, q_num = 0;
integer    beats_left = 0;
reg [28:0] beat_addr = 29'd0;
integer    busy_left = 0;

reg [63:0] joy_capture = 64'd0;
reg [63:0] set_capture = 64'd0;
integer    n_joy_wr = 0;
integer    n_set_wr = 0;
integer    n_vid_rd = 0;

// waitrequest: brief busy after each accepted command + queue backpressure.
// Quiescent-low matches the proven-stable hardware behaviour of the port.
assign m_wait = (q_num >= QD - 1) || (busy_left > 0);

function [63:0] rd_data;
	input [28:0] a;
	begin
		if (a == MB_ADDR)
`ifdef YUV_MB
			rd_data = 64'hE;                       // buffer A, YUV+intl+TFF
`else
			rd_data = 64'd0;                       // ARM request word: buffer A, BGR
`endif
		else if (a == CTL_ADDR)
			rd_data = {32'h4456_4433, 32'd0};      // DVD3 magic, src_std=0
		else if (a == JOY_ADDR)
			rd_data = joy_capture;
		else if (a == SET_ADDR)
			rd_data = set_capture;
		else
			rd_data = {2'b10, a, 4'h5, a};         // framebuffer pattern
	end
endfunction

always @(posedge clk) begin
	if (m_write && !m_wait) begin
		if (m_be != 8'd0) begin
			if (m_addr == JOY_ADDR) begin
				joy_capture = m_wdata;
				n_joy_wr = n_joy_wr + 1;
			end else if (m_addr == SET_ADDR) begin
				set_capture = m_wdata;
				n_set_wr = n_set_wr + 1;
			end
		end
		busy_left = 1 + (rnd(0) % 2);
	end else if (m_read && !m_wait) begin
		q_addr[q_tail] = m_addr;
		q_cnt[q_tail]  = m_cnt;
		q_tail = (q_tail + 1) % QD;
		q_num  = q_num + 1;
		if (m_addr < MB_ADDR)
			n_vid_rd = n_vid_rd + 1;
		busy_left = 1 + (rnd(0) % 2);
	end else if (busy_left > 0)
		busy_left = busy_left - 1;
end

// Read-burst delivery, independent of waitrequest, with random beat gaps.
always @(posedge clk) begin
	m_rvalid <= 0;
	if (beats_left == 0 && q_num > 0) begin
		beat_addr  = q_addr[q_head];
		beats_left = q_cnt[q_head];
		q_head = (q_head + 1) % QD;
		q_num  = q_num - 1;
	end
	if (beats_left > 0) begin
		if ((rnd(0) % 10) < 7) begin
			m_rvalid   <= 1;
			m_rdata    <= rd_data(beat_addr);
			beat_addr  = beat_addr + 29'd1;
			beats_left = beats_left - 1;
		end
	end
end

// Avalon hold-rule checker on the bridge side of the terminator:
// a command presented while waitrequest is high must persist unchanged.
// Terminator-generated dummy writes (byteenable==0) are excluded.
reg        chk_arm  = 0;
reg        chk_wr   = 0;
reg [28:0] chk_addr = 29'd0;
reg  [7:0] chk_cnt  = 8'd0;

always @(posedge clk) begin
	if (chk_arm) begin
		if (chk_wr ? !m_write : !m_read) begin
			n_viol = n_viol + 1;
			if (n_viol <= 10)
				$display("VIOLATION t=%0t: command withdrawn during waitrequest (wr=%0d addr=%h)",
				         $time, chk_wr, chk_addr);
		end else if (m_addr != chk_addr || m_cnt != chk_cnt) begin
			n_viol = n_viol + 1;
			if (n_viol <= 10)
				$display("VIOLATION t=%0t: command mutated during waitrequest", $time);
		end
	end
	chk_arm  <= (m_read | (m_write & (m_be != 8'd0))) & m_wait;
	chk_wr   <= m_write;
	chk_addr <= m_addr;
	chk_cnt  <= m_cnt;
end

// ------------------------------------------------------------- sequencing
task wait_cycles;
	input integer n;
	begin
		repeat (n) @(posedge clk);
	end
endtask

task check_alive;
	input integer idx;
	reg  [7:0] seq0;
	reg  [7:0] seqd;
	integer    joy0, vid0, i;
	integer    ok_seq, ok_vid, ok_joy;
	begin
		seq0 = dut.set_seq;
		joy0 = n_joy_wr;
		vid0 = n_vid_rd;
		ok_seq = 0; ok_vid = 0; ok_joy = 0;
		i = 0;
		while (i < 400 && !(ok_seq && ok_vid && ok_joy)) begin
			wait_cycles(1000);
			seqd = dut.set_seq - seq0;
			if (seqd >= 8'd2)            ok_seq = 1;
			if ((n_vid_rd - vid0) >= 20) ok_vid = 1;
			if (n_joy_wr > joy0)         ok_joy = 1;
			i = i + 1;
		end
		if (!(ok_seq && ok_vid && ok_joy)) begin
			fails = fails + 1;
			$display("FAIL cycle %0d: no recovery in 400k cycles: seq_ok=%0d vid_ok=%0d joy_ok=%0d (mb_st=%0d fb_st=%0d set_seq=%0d)",
			         idx, ok_seq, ok_vid, ok_joy, dut.mb_st, dut.fb_line_reader.st, dut.set_seq);
		end else
			$display("PASS cycle %0d: mailbox+video recovered (set_seq=%0d vid_rd=%0d joy_wr=%0d)",
			         idx, dut.set_seq, n_vid_rd, n_joy_wr);
	end
endtask

integer r;
integer hold;

`ifdef DBG
initial begin
	#1;
	forever begin
		wait_cycles(5000);
		$display("DBG t=%0t rst=%b fbst=%0d fill_need=%b mb_idle=%b quiet=%b pend=%0d rd_r=%b vid_active=%b mbst=%0d vc=%0d field=%b buf_ok=%b%b poll_due=%b joyp=%b",
		         $time, reset_req, dut.fb_line_reader.st, dut.fb_line_reader.fill_need,
		         dut.fb_line_reader.mb_idle, dut.ddr_quiet, dut.ddr_pend,
		         dut.fb_line_reader.ddr_rd_r, dut.vid_active, dut.mb_st,
		         dut.vc, dut.field, dut.fb_line_reader.buf_ok[0], dut.fb_line_reader.buf_ok[1],
		         dut.poll_due, dut.joy_pending);
	end
end
`endif

initial begin
	// Hardware registers power up to 0 on Cyclone V; mycore relies on that
	// for ce_pix (it only toggles). Deposit the power-up value for sim.
	dut.mycore.ce_pix = 1'b0;

	// Initial configuration: reset held from FPGA config until Main releases
	// it. The terminator is not yet armed (init_reset_deasserted=0).
	reset_req = 1;
	wait_cycles(3000);
	reset_req = 0;
	check_alive(0);

`ifdef YUV_MB
	begin
		integer pix_ok, pix_black, clocks, saw_yuv;
		pix_ok = 0; pix_black = 0; saw_yuv = 0;
		for (clocks = 0; clocks < 1200000; clocks = clocks + 1) begin
			wait_cycles(1);
			if (dut.fb_line_reader.display_yuv)
				saw_yuv = 1;
			if (!reset_req && dut.ce_pix == 1'b0 && dut.vc < 10'd288 &&
			    dut.hc < 10'd720) begin
				if (dut.pix_r != 8'd0 || dut.pix_g != 8'd0 || dut.pix_b != 8'd0)
					pix_ok = pix_ok + 1;
				else
					pix_black = pix_black + 1;
			end
		end
		$display("YUV_MB display_yuv=%0d pix_ok=%0d pix_black=%0d buf_ok=%0d%0d st=%0d pend=%0d plane=%0d",
		         dut.fb_line_reader.display_yuv, pix_ok, pix_black,
		         dut.fb_line_reader.buf_ok[0], dut.fb_line_reader.buf_ok[1],
		         dut.fb_line_reader.st, dut.ddr_pend, dut.fb_line_reader.plane_r);
		if (!saw_yuv) begin
			$display("FAIL YUV_MB: display_yuv never latched");
			fails = fails + 1;
		end
		if (pix_ok < 64) begin
			$display("FAIL YUV_MB: visible pixels %0d black=%0d", pix_ok, pix_black);
			fails = fails + 1;
		end else
			$display("OK   YUV_MB visible pixels %0d black=%0d", pix_ok, pix_black);
	end
`endif

`ifdef SHORT
	if (fails) begin
		$display("reset_safety_tb FAIL (%0d)", fails);
		$fatal(1);
	end
	$display("reset_safety_tb PASS (SHORT)");
	$finish;
`endif

	// Core reset pulses mid-activity: models Main app_restart()
	// fpga_core_reset(1) held across exec, then released by the next Main.
	// Random points hit the video FSM mid-burst and the mailbox FSM in
	// every state; random holds cover short and long reset windows.
	for (r = 1; r <= 8; r = r + 1) begin
		wait_cycles(20000 + (rnd(0) % 60000));
		reset_req = 1;
		hold = 200 + (rnd(0) % 40000);
		wait_cycles(hold);
		reset_req = 0;
		$display("-- reset cycle %0d released after %0d cycles held", r, hold);
		check_alive(r);
	end

`ifdef STRICT_AVALON
	if (n_viol != 0) begin
		$display("FAIL: %0d Avalon hold violations", n_viol);
		fails = fails + 1;
	end else
		$display("OK   no Avalon hold violations");
`else
	$display("INFO Avalon hold violations: %0d", n_viol);
`endif

	if (fails) begin
		$display("reset_safety_tb FAIL (%0d)", fails);
		$fatal(1);
	end
	$display("reset_safety_tb PASS");
	$finish;
end

endmodule
