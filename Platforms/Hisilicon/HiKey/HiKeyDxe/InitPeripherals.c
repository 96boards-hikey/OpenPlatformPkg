/** @file
*
*  Copyright (c) 2015, Linaro Ltd. All rights reserved.
*  Copyright (c) 2015, Hisilicon Ltd. All rights reserved.
*
*  This program and the accompanying materials
*  are licensed and made available under the terms and conditions of the BSD License
*  which accompanies this distribution.  The full text of the license may be found at
*  http://opensource.org/licenses/bsd-license.php
*
*  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
*  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
*
**/

#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#include <Hi6220.h>

#include "Hi6220RegsPeri.h"

STATIC
VOID
UsbPhyInit (
  IN VOID
  )
{
  UINT32 val, data;

  //setup clock
  MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_CLKEN0, BIT4);
  do {
    val = MmioRead32 (PERI_CTRL_BASE + SC_PERIPH_CLKSTAT0);
  } while ((val & BIT4) == 0);

  //setup phy
  data = RST0_USBOTG_BUS | RST0_POR_PICOPHY |
	  RST0_USBOTG | RST0_USBOTG_32K;
  MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_RSTDIS0, data);
  do {
    val = MmioRead32 (PERI_CTRL_BASE + SC_PERIPH_RSTSTAT0);
    val &= data;
  } while (val);

  val = MmioRead32 (PERI_CTRL_BASE + SC_PERIPH_CTRL4);
  val &= ~(CTRL4_PICO_SIDDQ | CTRL4_FPGA_EXT_PHY_SEL |
           CTRL4_OTG_PHY_SEL);
  val |=  CTRL4_PICO_VBUSVLDEXT | CTRL4_PICO_VBUSVLDEXTSEL;
  MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_CTRL4, val);
  MicroSecondDelay (1000);

  val = MmioRead32 (PERI_CTRL_BASE + SC_PERIPH_CTRL5);
  val &= ~CTRL5_PICOPHY_BC_MODE;
  MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_CTRL5, val);
  MicroSecondDelay (20000);
}

STATIC
VOID
UartInit (
  IN VOID
  )
{
  UINT32     Val;

  /* make UART1 out of reset */
  MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_RSTDIS3, PERIPH_RST3_UART1);
  MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_CLKEN3, PERIPH_RST3_UART1);
  /* make UART2 out of reset */
  MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_RSTDIS3, PERIPH_RST3_UART2);
  MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_CLKEN3, PERIPH_RST3_UART2);
  /* make UART3 out of reset */
  MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_RSTDIS3, PERIPH_RST3_UART3);
  MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_CLKEN3, PERIPH_RST3_UART3);
  /* make UART4 out of reset */
  MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_RSTDIS3, PERIPH_RST3_UART4);
  MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_CLKEN3, PERIPH_RST3_UART4);

  /* enable clock for BT/WIFI */
  Val = MmioRead32 (PMUSSI_REG(0x1c)) | 0x40;
  MmioWrite32 (PMUSSI_REG(0x1c), Val);
}

VOID
EFIAPI
HiKeyInitPeripherals (
  IN VOID
  )
{
  UINT32     Data, Bits;

  /* make I2C0/I2C1/I2C2/SPI0 out of reset */
  Bits = PERIPH_RST3_I2C0 | PERIPH_RST3_I2C1 | PERIPH_RST3_I2C2 | \
	 PERIPH_RST3_SSP;
  MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_RSTDIS3, Bits);

  do {
    Data = MmioRead32 (PERI_CTRL_BASE + SC_PERIPH_RSTSTAT3);
  } while (Data & Bits);

  UsbPhyInit ();
  UartInit ();
}
