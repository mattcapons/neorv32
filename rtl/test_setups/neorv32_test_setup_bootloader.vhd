-- ================================================================================ --
-- NEORV32 - Test Setup Using The UART-Bootloader To Upload And Run Executables     --
-- -------------------------------------------------------------------------------- --
-- The NEORV32 RISC-V Processor - https://github.com/stnolting/neorv32              --
-- Copyright (c) NEORV32 contributors.                                              --
-- Copyright (c) 2020 - 2026 Stephan Nolting. All rights reserved.                  --
-- Licensed under the BSD-3-Clause license, see LICENSE for details.                --
-- SPDX-License-Identifier: BSD-3-Clause                                            --
-- ================================================================================ --

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

library neorv32;
use neorv32.neorv32_package.all;

library accelerator;
use accelerator.systolic_pkg.all;

entity neorv32_test_setup_bootloader is
  generic (
    -- adapt these for your setup --
    CLOCK_FREQUENCY : natural := 100000000; -- clock frequency of clk_i in Hz
    IMEM_SIZE       : natural := 16*1024;   -- size of processor-internal instruction memory in bytes
    DMEM_SIZE       : natural := 64*1024     -- size of processor-internal data memory in bytes
  );
  port (
    -- Global control --
    clk_i       : in  std_ulogic; -- global clock, rising edge
    rstn_i      : in  std_ulogic; -- global reset, low-active, async
    -- GPIO --
    gpio_o      : out std_ulogic_vector(7 downto 0); -- parallel output
    -- UART0 --
    uart0_txd_o : out std_ulogic; -- UART0 send data
    uart0_rxd_i : in  std_ulogic  -- UART0 receive data
  );
end entity;

architecture neorv32_test_setup_bootloader_rtl of neorv32_test_setup_bootloader is

  signal con_gpio_out : std_ulogic_vector(31 downto 0);

  ----------------------------------------------------------------------------------------------
  -- ACCELERATOR DEDICATED
  ----------------------------------------------------------------------------------------------
  -- slink --
  type slink_t is record
    data  : std_ulogic_vector(31 downto 0); -- data
    valid : std_ulogic; -- source valid
    ready : std_ulogic; -- sink ready
  end record;
  signal slink_tx, slink_rx : slink_t;

  signal cfs_out : std_ulogic_vector(255 downto 0);
  signal acc_ready : std_ulogic;

begin

  -- The Core Of The Problem ----------------------------------------------------------------
  -- -------------------------------------------------------------------------------------------
  neorv32_top_inst: neorv32_top
  generic map (
    -- Clocking --
    CLOCK_FREQUENCY  => CLOCK_FREQUENCY,   -- clock frequency of clk_i in Hz
    -- Boot Configuration --
    BOOT_MODE_SELECT => 2,                 -- boot via internal bootloader
    -- RISC-V CPU Extensions --
    RISCV_ISA_C      => true,              -- implement compressed extension?
    RISCV_ISA_M      => true,              -- implement mul/div extension?
    RISCV_ISA_Zicntr => true,              -- implement base counters?
    -- Internal Instruction memory --
    IMEM_EN          => true,              -- implement processor-internal instruction memory
    IMEM_SIZE        => IMEM_SIZE, -- size of processor-internal instruction memory in bytes
    -- Internal Data memory --
    DMEM_EN          => true,              -- implement processor-internal data memory
    DMEM_SIZE        => DMEM_SIZE, -- size of processor-internal data memory in bytes
    -- Processor peripherals --
    IO_GPIO_NUM      => 8,                 -- number of GPIO input/output pairs (0..32)
    IO_CLINT_EN      => true,              -- implement core local interruptor (CLINT)?
    IO_UART0_EN      => true,               -- implement primary universal asynchronous receiver/transmitter (UART0)?

    IO_CFS_EN        => true,

    IO_SLINK_EN      => true,
    IO_SLINK_RX_FIFO => 16,
    IO_SLINK_TX_FIFO => 8
  )
  port map (
    -- Global control --
    clk_i       => clk_i,        -- global clock, rising edge
    rstn_i      => rstn_i,       -- global reset, low-active, async
    -- GPIO (available if IO_GPIO_NUM > 0) --
    gpio_o      => con_gpio_out, -- parallel output
    -- primary UART0 (available if IO_UART0_EN = true) --
    uart0_txd_o => uart0_txd_o,  -- UART0 send data
    uart0_rxd_i => uart0_rxd_i,   -- UART0 receive data

    -- Stream Link Interface --
    slink_rx_dat_i => slink_rx.data,
    slink_rx_val_i => slink_rx.valid,
    slink_rx_rdy_o => slink_rx.ready,
    slink_tx_dat_o => slink_tx.data,
    slink_tx_val_o => slink_tx.valid,
    slink_tx_rdy_i => slink_tx.ready,

    -- Custom Functions Subsystem IO --
    cfs_in_i       => (0 => acc_ready, others => '0'),
    cfs_out_o      => cfs_out
  );

  -----------------------------------------------------------------
  -- ACCELERATOR INSTANTIATION
  -----------------------------------------------------------------
  my_acc_inst : entity accelerator.acc_top
    port map(
        start_i     => cfs_out(0),
        clk_i       => clk_i,
        rstn_i      => rstn_i,
        acc_num_i   => cfs_out(32 downto 1),
        tx_vld_i    => slink_tx.valid,
        rx_rdy_i    => slink_rx.ready,
        tx_data_i   => slink_tx.data,
        tx_rdy_o    => slink_tx.ready,
        rx_vld_o    => slink_rx.valid,
        rdy_o       => acc_ready,
        rx_data_o   => slink_rx.data
    );

  -- GPIO output --
  gpio_o <= con_gpio_out(7 downto 0);

end architecture;
