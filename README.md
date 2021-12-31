# stm32f103_usb
Minimal usb-from-scratch serial port implementation and demo for STM32F103. 

This is a rather bare-bones environment for the STM32F103 "Blue Pill" platform.  

It requires nothing but arm-none-eabi-gcc (tested with v9 and v10) and GNU Make to build and
has no dependencies on outside code.

Features:
- entirely plain C, no assembly
- customized header declares all devices as true structures, with their absolute addresses provided in the linker script.
- no #defines or conditional compilations.  All constants are enums. 
- compact printf implementation based on stb_printf, which can be used with any device for which you provide a puts() function.
- extremely small usb stack (1.7k) providing a serial port compatible with most operating systems

To re-use: just cut, copy and paste. Premature Generalisation is The Root Of Much Complexity. 


KNOWN BUG:  
while on MacOS the device shows up as /dev/cu.usbmodem123456, on linux, this line:
    https://github.com/torvalds/linux/blob/master/drivers/usb/class/cdc-acm.c#L1299
seems to make the cdc_acm driver return cdc_acm: probe of 3-6:1.0 failed with error -22, and prevent
the appearance of /dev/ttyACMx. 

a workaround for is to run
  sudo modprobe usbserial vendor=0x0483 product=0x5740
and you'll have /dev/ttyUSBx instead.
  
i'll try to find a vendor/product combination that works on macos & linux out of the box. 
  
