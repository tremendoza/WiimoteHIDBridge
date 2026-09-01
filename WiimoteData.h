#pragma once
#include <stdint.h>

// Force the compiler to pack these structs byte-for-byte with no padding
#pragma pack(push, 1)

struct ButtonData {
    uint16_t hex;

    bool Left() const { return hex & 0x0001; }
    bool Right() const { return hex & 0x0002; }
    bool Down() const { return hex & 0x0004; }
    bool Up() const { return hex & 0x0008; }
    bool Plus() const { return hex & 0x0010; }

    bool Two() const { return hex & 0x0100; }
    bool One() const { return hex & 0x0200; }
    bool B() const { return hex & 0x0400; }
    bool A() const { return hex & 0x0800; }
    bool Minus() const { return hex & 0x1000; }
    bool Home() const { return hex & 0x8000; }

    uint8_t AccelBits1() const { return (hex & 0x0060) >> 5; }
    uint8_t AccelBits2() const { return (hex & 0x6000) >> 13; }
};

struct IRBasic {
    uint8_t x1;
    uint8_t y1;
    uint8_t x2hi : 2;
    uint8_t y2hi : 2;
    uint8_t x1hi : 2;
    uint8_t y1hi : 2;
    uint8_t x2;
    uint8_t y2;

    uint16_t GetX1() const { return (x1hi << 8) | x1; }
    uint16_t GetY1() const { return (y1hi << 8) | y1; }
    uint16_t GetX2() const { return (x2hi << 8) | x2; }
    uint16_t GetY2() const { return (y2hi << 8) | y2; }
};

struct MotionPlusData {
    uint8_t yaw1;
    uint8_t roll1;
    uint8_t pitch1;
    uint8_t pitch_slow : 1;
    uint8_t yaw_slow : 1;
    uint8_t yaw2 : 6;
    uint8_t extension_connected : 1;
    uint8_t roll_slow : 1;
    uint8_t roll2 : 6;
    uint8_t zero : 1;
    uint8_t is_mp_data : 1;
    uint8_t pitch2 : 6;

    uint16_t GetYaw() const { return (static_cast<uint16_t>(yaw2) << 8) | yaw1; }
    uint16_t GetRoll() const { return (static_cast<uint16_t>(roll2) << 8) | roll1; }
    uint16_t GetPitch() const { return (static_cast<uint16_t>(pitch2) << 8) | pitch1; }
};

struct NunchukData {
    uint8_t sx;             // Joystick X
    uint8_t sy;             // Joystick Y
    uint8_t ax_msb;         // Accel X MSB
    uint8_t ay_msb;         // Accel Y MSB
    uint8_t az_msb;         // Accel Z MSB
    uint8_t z : 1;          // Z Button (Active Low)
    uint8_t c : 1;          // C Button (Active Low)
    uint8_t ax_lsb : 2;     // Accel X LSB
    uint8_t ay_lsb : 2;     // Accel Y LSB
    uint8_t az_lsb : 2;     // Accel Z LSB

    bool Z() const { return !(z); }
    bool C() const { return !(c); }
    uint16_t AccelX() const { return (ax_msb << 2) | ax_lsb; }
    uint16_t AccelY() const { return (ay_msb << 2) | ay_lsb; }
    uint16_t AccelZ() const { return (az_msb << 2) | az_lsb; }
};

// Master struct for Report 0x37
struct InputReport37 {
    uint8_t report_id; // Will be 0x37
    ButtonData buttons;
    uint8_t accel_x_msb;
    uint8_t accel_y_msb;
    uint8_t accel_z_msb;
    IRBasic ir_dots_1_2;
    IRBasic ir_dots_3_4;
    union {
        MotionPlusData gyro;
        NunchukData nunchuk;
        uint8_t raw[6];
    } ext;
    uint8_t battery; // Battery level byte (0x00 - 0xFF) received from Status Report 0x20
};

#pragma pack(pop)
