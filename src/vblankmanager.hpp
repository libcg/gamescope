// Try to figure out when vblank is and notify steamcompmgr to render some time before it

int vblank_init( void );

void vblank_mark_possible_vblank( uint64_t nanos );

extern std::atomic<uint64_t> g_uVblankDrawTimeNS;

const unsigned int g_uDefaultVBlankRedZone = 1'650'000;
const unsigned int g_uDefaultMinVBlankTime = 350'000; // min vblank time for fps limiter to care about
const unsigned int g_uDefaultVBlankRateOfDecayPercentage = 980;

extern uint64_t g_uVblankDrawBufferRedZoneNS;
extern uint64_t g_uVBlankRateOfDecayPercentage;
