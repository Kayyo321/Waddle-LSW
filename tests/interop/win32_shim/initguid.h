#ifndef WaddleTestInitguidH
#define WaddleTestInitguidH
/** @brief Minimal DEFINE_GUID adapter for the pinned vendor header in Linux tests. */
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    const GUID name = { (uint32_t)(l), (uint16_t)(w1), (uint16_t)(w2), \
        { (uint8_t)(b1), (uint8_t)(b2), (uint8_t)(b3), (uint8_t)(b4), \
          (uint8_t)(b5), (uint8_t)(b6), (uint8_t)(b7), (uint8_t)(b8) } }
#endif
