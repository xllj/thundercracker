/* -*- mode: C; c-basic-offset: 4; intent-tabs-mode: nil -*-
 *
 * Sifteo Thundercracker simulator
 * Micah Elizabeth Scott <micah@misc.name>
 *
 * Copyright <c> 2011 Sifteo, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "cube_hardware.h"
#include "cube_debug.h"
#include "cube_cpu_callbacks.h"

namespace Cube {

bool Hardware::init(VirtualTime *masterTimer, const char *firmwareFile,
    FlashStorage::CubeRecord *flashStorage)
{
    time = masterTimer;
    hwDeadline.init(time);

    lat1 = 0;
    lat2 = 0;
    bus = 0;
    prev_ctrl_port = 0;
    exceptionCount = 0;
    
    memset(&cpu, 0, sizeof cpu);
    cpu.callbackData = this;
    cpu.vtime = masterTimer;
    
    CPU::em8051_reset(&cpu, true);

    if (firmwareFile) {
        if (CPU::em8051_load(&cpu, firmwareFile)) {
            fprintf(stderr, "Error: Failed to load firmware '%s'\n", firmwareFile);
            return false;
        }
    } else {
        CPU::em8051_init_sbt(&cpu);
    }

    flash.init(flashStorage);
    spi.radio.init(&cpu);
    spi.init(&cpu);
    adc.init();
    mdu.init();
    i2c.init();
    lcd.init();
    rng.init();
    neighbors.init();
    
    setTouch(false);
    
    // XXX: Simulated battery level
    i2c.accel.setADC1(0x8760);
    
    return true;
}

uint64_t Hardware::getHWID() const
{
    /*
     * Read the HWID straight from the cube's NVM. May return ~0 if
     * the cube has not yet initialized its own HWID.
     */

    uint64_t result;
    memcpy(&result, flash.getStorage()->nvm, sizeof result);
    return result;
}

void Hardware::reset()
{
    CPU::em8051_reset(&cpu, false);
}

void Hardware::fullReset()
{
    // Reset the contents of flash memory as well
    FlashStorage::CubeRecord *rec = flash.getStorage();
    memset(rec->nvm, 0xFF, sizeof rec->nvm);
    memset(rec->ext, 0xFF, sizeof rec->ext);
    reset();
}

// cube_cpu_callbacks.h
void CPU::except(CPU::em8051 *cpu, int exc)
{
    Hardware *self = (Hardware*) cpu->callbackData;
    const char *name = CPU::em8051_exc_name(exc);
    
    self->incExceptionCount();

    Tracer::log(cpu, "@%04x EXCEPTION: %s", cpu->mPC, name);

    if (self == Cube::Debug::cube && Cube::Debug::stopOnException)
        Cube::Debug::emu_exception(cpu, exc);
    else
        fprintf(stderr, "[%2d] EXCEPTION at 0x%04x: %s\n", cpu->id, cpu->mPC, name);
}

// cube_cpu_callbacks.h
int CPU::NVM::write(CPU::em8051 *cpu, uint16_t addr, uint8_t data)
{
    Hardware *self = (Hardware*) cpu->callbackData;

    if (!(cpu->mSFR[REG_FSR] & (1<<5))) {
        // Write disabled
        except(cpu, EXCEPTION_NVM);
        return 0;
    }

    // Program flash bits (1 -> 0)
    ASSERT(addr < sizeof self->flash.getStorage()->nvm);
    self->flash.getStorage()->nvm[addr] &= data;
    
    // Self-timed write cycles
    return 12800;
}

// cube_cpu_callbacks.h
uint8_t CPU::NVM::read(CPU::em8051 *cpu, uint16_t addr)
{
    Hardware *self = (Hardware*) cpu->callbackData;

    ASSERT(addr < sizeof self->flash.getStorage()->nvm);
    return self->flash.getStorage()->nvm[addr];
}

void Hardware::sfrWrite(int reg)
{
    CPU::SFR::writeInline(this, &cpu, reg);
}

int Hardware::sfrRead(int reg)
{
    return CPU::SFR::readInline(this, &cpu, reg);
}

void Hardware::debugByte()
{
     printf("DEBUG[%d]: %02x\n", cpu.id, cpu.mSFR[REG_DEBUG]);
}

void Hardware::graphicsTick()
{
    /*
     * Update the graphics (LCD and Flash) bus. Only happens in
     * response to relevant I/O port changes, not on every clock tick.
     */
    
    // Port output values, pull-up when floating
    uint8_t bus_port = cpu.mSFR[BUS_PORT] | cpu.mSFR[BUS_PORT_DIR];
    uint8_t addr_port = cpu.mSFR[ADDR_PORT] | cpu.mSFR[ADDR_PORT_DIR];
    uint8_t ctrl_port = cpu.mSFR[CTRL_PORT] | cpu.mSFR[CTRL_PORT_DIR];;

    // 7-bit address in high bits of p1
    uint8_t addr7 = addr_port >> 1;

    // Bit A21 comes from the accelerometer's INT2 pin
    bool a21 = i2c.accel.intPin(1);

    // Is the MCU driving any bit of the shared bus?
    uint8_t mcu_data_drv = cpu.mSFR[BUS_PORT_DIR] != 0xFF;

    Flash::Pins flashp = {
        /* addr    */ addr7 | ((uint32_t)lat1 << 7) | ((uint32_t)lat2 << 14) | ((uint32_t)a21 << 21),
        /* power   */ ctrl_port & CTRL_DS_EN,
        /* oe      */ ctrl_port & CTRL_FLASH_OE,
        /* ce      */ 0,
        /* we      */ ctrl_port & CTRL_FLASH_WE,
        /* data_in */ bus,
    };

    LCD::Pins lcdp = {
        /* power   */ ctrl_port & CTRL_3V3_EN,
        /* csx     */ 0,
        /* dcx     */ ctrl_port & CTRL_LCD_DCX,
        /* wrx     */ addr_port & 1,
        /* rdx     */ 0,
        /* data_in */ bus,
    };

    flash.cycle(&flashp, &cpu);
    lcd.cycle(&lcdp);

    /* Backlight latch */
    if ((ctrl_port & CTRL_FLASH_LAT1) && !(prev_ctrl_port & CTRL_FLASH_LAT1)) {
        const uint8_t mask = CTRL_3V3_EN | CTRL_LCD_DCX;
        backlight.cycle(mask == (ctrl_port & mask), time->clocks);
    }

    /* Address latch write cycles, triggered by rising edge */

    if ((ctrl_port & CTRL_FLASH_LAT1) && !(prev_ctrl_port & CTRL_FLASH_LAT1)) lat1 = addr7;
    if ((ctrl_port & CTRL_FLASH_LAT2) && !(prev_ctrl_port & CTRL_FLASH_LAT2)) lat2 = addr7;
    prev_ctrl_port = ctrl_port;

    /*
     * After every simulation cycle, resolve the new state of the
     * shared bus.  We update the bus once now, but flash memory may
     * additionally update more often (every tick).
     */
    
    switch ((mcu_data_drv << 1) | flashp.data_drv) {
    case 0:     /* Floating... */ break;
    case 1:     bus = flash.dataOut(); break;
    case 2:     bus = bus_port; break;
    default:
        /* Bus contention! */
        CPU::except(&cpu, CPU::EXCEPTION_BUS_CONTENTION);
    }
    
    flash_drv = flashp.data_drv;  
    cpu.mSFR[BUS_PORT] = bus;
}

void Hardware::setAcceleration(float xG, float yG, float zG)
{
    /*
     * Set the cube's current acceleration, in G's. Scale it
     * according to the accelerometer's maximum range (assuming
     * the firmware has it configured for a full scale of +/- 2g).
     */

    i2c.accel.setVector(scaleAccelAxis(xG), scaleAccelAxis(yG), scaleAccelAxis(zG));
}

int16_t Hardware::scaleAccelAxis(float g)
{
    /*
     * Scale a raw acceleration, in G's, and return the corresponding
     * two's complement accelerometer reading.
     *
     * Simulates some of our non-ideal behavior, such as saturation at
     * the extremes, and a little bit of noise.
     */

    const int range = 1 << 15;
    const float fullScale = 2.0f;
    const int noiseAmount = 0x60;  // A little less than 1 LSB after truncation

    unsigned randomBits = rand();
    int noise = ((randomBits & 0xFFFF) * noiseAmount) >> 16;
    if ((randomBits >> 16) & 1)
        noise = -noise;

    int scaled = g * (range / fullScale) + noise;
    int16_t truncated = scaled;

    if (scaled != truncated)
        truncated = scaled > 0 ? range - 1 : -range;
        
    return truncated;
}



NEVER_INLINE void Hardware::hwDeadlineWork() 
{
    cpu.needHardwareTick = false;
    hwDeadline.reset();

    lcd.tick(hwDeadline, &cpu);
    adc.tick(hwDeadline, &cpu);
    spi.tick(hwDeadline, cpu.mSFR + REG_SPIRCON0, &cpu);
    i2c.tick(hwDeadline, &cpu);
    flash.tick(hwDeadline, &cpu);
    spi.radio.tick(rfcken, &cpu);
}

void Hardware::setTouch(bool touching)
{
    if (touching)
        cpu.mSFR[MISC_PORT] |= MISC_TOUCH;
    else
        cpu.mSFR[MISC_PORT] &= ~MISC_TOUCH;
}

bool Hardware::isDebugging()
{
    return this == Cube::Debug::cube;
}

uint32_t Hardware::getExceptionCount()
{
    return exceptionCount;
}

void Hardware::incExceptionCount()
{
    exceptionCount++;
}

void Hardware::logWatchdogReset()
{
    /*
     * The watchdog timer expired. We don't treat this as a normal
     * CPU exception, since the overall behaviour of reset recovery
     * after a WDT fault is something that we need to accurately
     * simulate, both for unit testing and for simulating userspace
     * code which may happen to cause a watchdog fault due to a bad
     * flash loadstream.
     *
     * This is called by the CPU emulation core, which is about to handle
     * a reset. We can log a little extra info about the fault, to make
     * debugging easier.
     *
     * If this fault was caused by a flash verify error, the bus address
     * and data will match what we're seeing on the flash device, and "a"
     * will have the expected value that isn't matching.
     */

    Tracer::logV(&cpu,
        "CUBE[%d]: Watchdog reset. pc=%02x bus=[%02x.%02x.%02x -> %02x] a=%02x\n",
        cpu.id, cpu.mPC,
        lat2, lat1, cpu.mSFR[ADDR_PORT],
        cpu.mSFR[BUS_PORT], cpu.mSFR[REG_ACC]);
}

bool Hardware::testWakeOnPin()
{
    /*
     * Accelerometer INT2 drives LAT1 through a 10K resistor, for wakeup purposes.
     * We need to check this when asleep, so we can't update it soleley in graphicsTick().
     */

    if (cpu.mSFR[CTRL_PORT_DIR] & CTRL_FLASH_LAT1) {
        if (i2c.accel.intPin(1))
            cpu.mSFR[CTRL_PORT] |= CTRL_FLASH_LAT1;
        else
            cpu.mSFR[CTRL_PORT] &= ~CTRL_FLASH_LAT1;
    }

    // Check wake-on-pin
    uint8_t c0 = cpu.mSFR[REG_WUOPC0];
    uint8_t c1 = cpu.mSFR[REG_WUOPC1];
    uint8_t p0 = cpu.mSFR[REG_P2];
    uint8_t p1 = (cpu.mSFR[REG_P1] & 0x80) | (cpu.mSFR[REG_P3] & 0x7F);

    return ((c0 & p0) | (c1 & p1));
}

void Hardware::traceExecution()
{
    uint8_t bank = (cpu.mSFR[REG_PSW] & (PSWMASK_RS0|PSWMASK_RS1)) >> PSW_RS0;

    char assembly[128];
    CPU::em8051_decode(&cpu, cpu.mPC, assembly);

    Tracer::logV(&cpu,
        "@%04X i%d a%02X reg%d[%02X%02X%02X%02X-%02X%02X%02X%02X] "
        "dptr%d[%04X%04X] port[%02X%02X%02X%02X-%02X%02X%02X%02X] "
        "lat[%02x.%02x] wdt%d[%06x] tmr[%02X%02X%02X%02X%02X%02X] "
        "rtc[%04x-%02x%02x]  %s",

        cpu.mPC, cpu.irq_count,
        cpu.mSFR[REG_ACC],

        // reg
        bank,
        cpu.mData[bank*8 + 0],
        cpu.mData[bank*8 + 1],
        cpu.mData[bank*8 + 2],
        cpu.mData[bank*8 + 3],
        cpu.mData[bank*8 + 4],
        cpu.mData[bank*8 + 5],
        cpu.mData[bank*8 + 6],
        cpu.mData[bank*8 + 7],

        // dptr
        cpu.mSFR[REG_DPS] & 1,
        (cpu.mSFR[REG_DPH] << 8) | cpu.mSFR[REG_DPL],
        (cpu.mSFR[REG_DPH1] << 8) | cpu.mSFR[REG_DPL1],

        // port
        cpu.mSFR[REG_P0],
        cpu.mSFR[REG_P1],
        cpu.mSFR[REG_P2],
        cpu.mSFR[REG_P3],
        cpu.mSFR[REG_P0DIR],
        cpu.mSFR[REG_P1DIR],
        cpu.mSFR[REG_P2DIR],
        cpu.mSFR[REG_P3DIR],

        // lat
        lat2, lat1,

        // wdt
        cpu.wdtEnabled,
        cpu.wdtCounter,

        // tmr
        cpu.mSFR[REG_TH0],
        cpu.mSFR[REG_TL0],
        cpu.mSFR[REG_TH1],
        cpu.mSFR[REG_TL1],
        cpu.mSFR[REG_TH2],
        cpu.mSFR[REG_TL2],

        // rtc
        cpu.rtc2,
        cpu.mSFR[REG_RTC2CMP1],
        cpu.mSFR[REG_RTC2CMP0],

        assembly);
}

void Hardware::initVCD(VCDWriter &vcd)
{
    /*
     * Set up trace variables for our VCD file output.
     *
     * This feature acts like a built-in logic analyzer;
     * during initialization we identify interesting variables,
     * then they're monitored during each clock tick (when tracing
     * is enabled) and the results are written to an industry
     * standard VCD file.
     */
     
    vcd.enterScope("gpio"); {
    
        // Parallel busses
        vcd.define("addr", &cpu.mSFR[ADDR_PORT], 8); 
        vcd.define("addr_dir", &cpu.mSFR[ADDR_PORT_DIR], 8); 
        vcd.define("bus", &cpu.mSFR[BUS_PORT], 8); 
        vcd.define("bus_dir", &cpu.mSFR[BUS_PORT_DIR], 8); 
    
        // Ctrl port, broken out
        vcd.define("lcd_dcx", &cpu.mSFR[CTRL_PORT], 1, 0);
        vcd.define("flash_lat1", &cpu.mSFR[CTRL_PORT], 1, 1);
        vcd.define("flash_lat2", &cpu.mSFR[CTRL_PORT], 1, 2);
        vcd.define("en3v3", &cpu.mSFR[CTRL_PORT], 1, 3);
        vcd.define("ds_en", &cpu.mSFR[CTRL_PORT], 1, 4);
        vcd.define("flash_we", &cpu.mSFR[CTRL_PORT], 1, 5);
        vcd.define("flash_oe", &cpu.mSFR[CTRL_PORT], 1, 6);
        vcd.define("ctrl_dir", &cpu.mSFR[CTRL_PORT_DIR], 8); 

        // Misc port, broken out
        vcd.define("nb_top", &cpu.mSFR[MISC_PORT], 1, Neighbors::PIN_0_TOP_IDX);
        vcd.define("nb_top_dir", &cpu.mSFR[MISC_PORT_DIR], 1, Neighbors::PIN_0_TOP_IDX);
        vcd.define("nb_left", &cpu.mSFR[MISC_PORT], 1, Neighbors::PIN_1_LEFT_IDX);
        vcd.define("nb_left_dir", &cpu.mSFR[MISC_PORT_DIR], 1, Neighbors::PIN_1_LEFT_IDX);
        vcd.define("nb_bottom", &cpu.mSFR[MISC_PORT], 1, Neighbors::PIN_2_BOTTOM_IDX);
        vcd.define("nb_bottom_dir", &cpu.mSFR[MISC_PORT_DIR], 1, Neighbors::PIN_2_BOTTOM_IDX);
        vcd.define("nb_right", &cpu.mSFR[MISC_PORT], 1, Neighbors::PIN_3_RIGHT_IDX);
        vcd.define("nb_right_dir", &cpu.mSFR[MISC_PORT_DIR], 1, Neighbors::PIN_3_RIGHT_IDX);
        
        /*
         * Neighbor IN is sampled from t012 instead of MISC_PORT,
         * since the corresponding bit in MISC_PORT is cleared by
         * Neighbors::clearNeighborInput before we can read it.
         */
        vcd.define("nb_in", &cpu.t012, 1, 6);
        vcd.define("nb_in_dir", &cpu.mSFR[MISC_PORT_DIR], 1, 6);

    } vcd.leaveScope();
    
    vcd.enterScope("cpu"); {
        // Internal state
        vcd.define("irq_count", &cpu.irq_count, 3);
        vcd.define("PC", &cpu.mPC, 16);

        // Important registers
        vcd.define("TL0", &cpu.mSFR[REG_TL0], 8); 
        vcd.define("TH0", &cpu.mSFR[REG_TH0], 8); 
        vcd.define("TL1", &cpu.mSFR[REG_TL1], 8); 
        vcd.define("TH1", &cpu.mSFR[REG_TH1], 8); 
        vcd.define("TL2", &cpu.mSFR[REG_TL2], 8); 
        vcd.define("TH2", &cpu.mSFR[REG_TH2], 8); 
        vcd.define("TCON", &cpu.mSFR[REG_TCON], 8); 
        vcd.define("IRCON", &cpu.mSFR[REG_IRCON], 8); 
        vcd.define("SP", &cpu.mSFR[REG_SP], 8); 
        vcd.define("DEBUG", &cpu.mSFR[REG_DEBUG], 8); 
    } vcd.leaveScope();
    
    vcd.enterScope("radio"); {
        spi.radio.initVCD(vcd);
    } vcd.leaveScope();        
}


};  // namespace Cube
