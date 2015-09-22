# i2c-ns9xxx
Modified I2C driver for the Digi NS9xxx platform (e.g. the NS9210 and NS9215 ARM9 SOC modules).


While testing with a Digi Connect ME 9210 module, I2C-communications turned out to be unstable. The bus would lock up after a few hours. It turned out that the I2C bus speed was not set correctly by the Digi Linux driver. Moreover, the original driver did not implement error recovery, status checking and locking very well, and hardly produced useful output for debugging.


This is an attempt to fix the main issues with the Digi NS9xxx I2C-driver. Among other things, the following modifications have been made:

 - Lower the default value of the SCL_DELAY parameter (from 306 to 25) 
   in order to get a sane bus speed, and make the value configurable with 
   the scl_delay driver parameter.
 - Attempt to restore the I2C bus-state after a lockup by executing a bus-reset:
    - Add ns9xxx_i2c_reset_bitbang() to reset the bus using GPIO bitbanging
    - Add ns9xxx_reinit_i2c() to reset the bus and reinitialise the I2C hardware
    - If the master command module stays locked, call ns9xxx_reinit_i2c()
 - Dump extensive debugging information to the kernel logs.
 - Improve locking and interrupt handling:
    - Set a spinlock for variable-assignments in the interrupt handler
    - Set a spinlock while setting state and command registers in ns9xxx_send_cmd()
    - Disable interrupt when doing GPIO bitbanging
    - In ns9xxx_i2c_xfer(), set a spinlock while setting the master address
 - Improve status checking:
    - Before sending a command, check if the master command module is locked
    - Add ns9xxx_wait_while_busy() to wait for the hardware to unlock
 - Make naming more consistent with the NS9xxx hardware manual


### Further reading:

 - [Digi NS9210 Processor Module Hardware Reference](http://ftp1.digi.com/support/documentation/90001002_A.pdf)
 - [Digi NS9210 Hardware Reference Manual](http://www.digi.com/pdf/prd_ds_ns9210_hwman.pdf)
 - [Digi Connect ME and Wi-ME Hardware Reference](http://www.digi.com/pdf/prd_ds_digiconnectme_hwman.pdf)
