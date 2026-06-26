#include <genesis.h>

// ---------------------------------------------------------------------------
// Grid dimensions (one tile per cell, 8x8 px each => 40x28 fills 320x224)
// ---------------------------------------------------------------------------
#define GRID_W 40
#define GRID_H 28

// ---------------------------------------------------------------------------
// VRAM tile slots  (TILE_USER_INDEX is the first safe user slot in SGDK)
// ---------------------------------------------------------------------------
#define TILE_IDX_DEAD           TILE_USER_INDEX          // dark grey  – dead cell
#define TILE_IDX_LIVE           (TILE_USER_INDEX + 1)    // yellow     – live cell
#define TILE_IDX_CURSOR_EMPTY   (TILE_USER_INDEX + 2)    // blue border, grey interior  – cursor on dead cell
#define TILE_IDX_CURSOR_FILLED  (TILE_USER_INDEX + 3)    // blue border, yellow interior – cursor on live cell

// ---------------------------------------------------------------------------
// Palette colour indices  (all in PAL0)
// Entry 0 is the global backdrop / transparent – we reuse it as dead-cell bg
// ---------------------------------------------------------------------------
#define COL_BG     0   // light grey backdrop (PAL0 entry 0)
#define COL_GRID   1   // darker grey grid lines / dead cell fill
#define COL_LIVE   2   // yellow live cell
#define COL_CURSOR 3   // blue cursor border

// ---------------------------------------------------------------------------
// Tile attribute helpers
// TILE_ATTR_FULL(palette, priority, vflip, hflip, vram_index)
// ---------------------------------------------------------------------------
#define ATTR_DEAD           TILE_ATTR_FULL(PAL0, 0, FALSE, FALSE, TILE_IDX_DEAD)
#define ATTR_LIVE           TILE_ATTR_FULL(PAL0, 0, FALSE, FALSE, TILE_IDX_LIVE)
#define ATTR_CURSOR_EMPTY   TILE_ATTR_FULL(PAL0, 0, FALSE, FALSE, TILE_IDX_CURSOR_EMPTY)
#define ATTR_CURSOR_FILLED  TILE_ATTR_FULL(PAL0, 0, FALSE, FALSE, TILE_IDX_CURSOR_FILLED)

// ---------------------------------------------------------------------------
// Solid-colour 8x8 tile data  (4bpp packed, 8 rows × 2 pixels/nibble)
// Each u32 encodes one row of 8 pixels; each nibble = one palette index.
// A solid tile filled with palette index N has every nibble == N.
// ---------------------------------------------------------------------------

// Dark grey dead/grid tile  – palette index 1 → nibble 0x1 → row = 0x11111111
static const u32 tile_dead[8] = {
    0x11111111, 0x11111111, 0x11111111, 0x11111111,
    0x11111111, 0x11111111, 0x11111111, 0x11111111
};

// Yellow live cell tile  – palette index 2 → nibble 0x2 → row = 0x22222222
static const u32 tile_live[8] = {
    0x22222222, 0x22222222, 0x22222222, 0x22222222,
    0x22222222, 0x22222222, 0x22222222, 0x22222222
};

// Blue-bordered cursor tile, empty interior (palette index 0 = light grey backdrop)
// Row layout for an 8x8 tile:
//   Top/bottom rows:  all blue (index 3) → 0x33333333
//   Middle rows: blue edges, backdrop interior → 0x30000003
static const u32 tile_cursor_empty[8] = {
    0x33333333,   // top border
    0x30000003,
    0x30000003,
    0x30000003,
    0x30000003,
    0x30000003,
    0x30000003,
    0x33333333    // bottom border
};

// Blue-bordered cursor tile, filled interior (palette index 2 = yellow)
//   Top/bottom rows:  all blue (index 3) → 0x33333333
//   Middle rows: blue edges, yellow interior → 0x32222223
static const u32 tile_cursor_filled[8] = {
    0x33333333,   // top border
    0x32222223,
    0x32222223,
    0x32222223,
    0x32222223,
    0x32222223,
    0x32222223,
    0x33333333    // bottom border
};

// ---------------------------------------------------------------------------
// Game state
// ---------------------------------------------------------------------------
static u8 grid[GRID_W][GRID_H];       // current generation  (0 = dead, 1 = live)
static u8 next[GRID_W][GRID_H];       // scratch buffer for next generation

static u16 cursor_x;                   // cursor column  [0, GRID_W)
static u16 cursor_y;                   // cursor row     [0, GRID_H)
static u8  running;                    // 1 = simulation running, 0 = paused
static u8  in_menu;                    // 1 = showing main menu, 0 = in game

// ---------------------------------------------------------------------------
// SRAM layout
//   Offset 0   : u16 magic number  (0xC0DE = valid save present)
//   Offset 2   : u8  grid[GRID_W][GRID_H]  (40*28 = 1120 bytes, column-major)
//   Total      : 1122 bytes (well within the 8KB minimum Genesis SRAM)
//
// The magic number lets us detect a blank/uninitialised SRAM on first boot
// so we never hydrate the grid with garbage data.
// ---------------------------------------------------------------------------
#define SRAM_MAGIC          0xC0DE
#define SRAM_OFFSET_MAGIC   0                               // u16 → 2 bytes
#define SRAM_OFFSET_GRID    2                               // u8[GRID_W*GRID_H]

// Save the current grid to SRAM.
// Called on pause and on each cell toggle.
static void sram_save(void)
{
    u16 x, y;
    u16 offset;

    SRAM_enable();

    // Write magic number so we know this save slot is valid
    SRAM_writeWord(SRAM_OFFSET_MAGIC, SRAM_MAGIC);

    // Write grid column-major (matching grid[x][y] memory layout)
    offset = SRAM_OFFSET_GRID;
    for (x = 0; x < GRID_W; x++)
    {
        for (y = 0; y < GRID_H; y++)
        {
            SRAM_writeByte(offset, grid[x][y]);
            offset++;
        }
    }

    SRAM_disable();
}

// Load the grid from SRAM.
// Returns 1 if a valid save was found and loaded, 0 if SRAM was blank/invalid.
static u8 sram_load(void)
{
    u16 x, y;
    u16 offset;
    u16 magic;

    SRAM_enable();
    magic = SRAM_readWord(SRAM_OFFSET_MAGIC);
    SRAM_disable();

    if (magic != SRAM_MAGIC)
        return 0;   // no valid save — leave grid as-is

    SRAM_enable();

    offset = SRAM_OFFSET_GRID;
    for (x = 0; x < GRID_W; x++)
    {
        for (y = 0; y < GRID_H; y++)
        {
            // Mask to 1-bit so corrupt SRAM bytes can't break simulation logic
            grid[x][y] = SRAM_readByte(offset) & 1;
            offset++;
        }
    }

    SRAM_disable();
    return 1;
}

// ---------------------------------------------------------------------------
// Flags set by the joypad callback, consumed in the main loop
static volatile u8 flag_start;
static volatile u8 flag_toggle_cell;
static volatile u8 flag_move_left;
static volatile u8 flag_move_right;
static volatile u8 flag_move_up;
static volatile u8 flag_move_down;

// ---------------------------------------------------------------------------
// Joypad event callback
// 'changed' bits: button state changed since last poll
// 'state'   bits: current pressed state
// We react on the PRESS edge (bit in changed AND bit in state).
// ---------------------------------------------------------------------------
static void joy_handler(u16 joy, u16 changed, u16 state)
{
    if (joy != JOY_1) return;

    // Start – toggle on press
    if ((changed & BUTTON_START) && (state & BUTTON_START))
        flag_start = 1;

    // Cell toggle – A, B, or C on press
    if ((changed & BUTTON_A) && (state & BUTTON_A)) flag_toggle_cell = 1;
    if ((changed & BUTTON_B) && (state & BUTTON_B)) flag_toggle_cell = 1;
    if ((changed & BUTTON_C) && (state & BUTTON_C)) flag_toggle_cell = 1;

    // D-pad – each direction on press
    if ((changed & BUTTON_LEFT)  && (state & BUTTON_LEFT))  flag_move_left  = 1;
    if ((changed & BUTTON_RIGHT) && (state & BUTTON_RIGHT)) flag_move_right = 1;
    if ((changed & BUTTON_UP)    && (state & BUTTON_UP))    flag_move_up    = 1;
    if ((changed & BUTTON_DOWN)  && (state & BUTTON_DOWN))  flag_move_down  = 1;
}

// ---------------------------------------------------------------------------
// Draw the main menu onto BG_A.
//
// BG_A sits on top of BG_B, and VDP_drawText writes to BG_A by default.
// The system font renders white glyphs on transparent tiles, so the grey
// game grid on BG_B shows through as the background — no extra fill needed.
//
// Screen is 40 columns × 28 rows (each tile = 8×8 px).
//
// Layout (all rows are 0-based):
//   Row  4      – spaced title  "C O N W A Y ' S  G A M E  O F  L I F E"
//   Row  5      – spaced title  "          (blank, part of title weight)"
//   Row  8      – controls header  "Controls:"
//   Row 10      – "A / B / C  -  Toggle cell on or off"
//   Row 11      – "  D-Pad   -  Move cursor"
//   Row 12      – "  Start   -  Run / Pause simulation"
//   Row 20      – "- Press Start to Begin -"
// ---------------------------------------------------------------------------
static void draw_menu(void)
{
    // ------------------------------------------------------------------
    // Title: spaced letters to simulate a larger, bolder look.
    // String: "C O N W A Y ' S  G A M E  O F  L I F E"
    // Length: 39 characters  →  start col = (40 - 39) / 2 = 0 (left edge)
    // We nudge to col 1 to keep 1-cell margin on each side for a 38-char
    // version: "C O N W A Y ' S  G A M E  O F  L I F E" is actually 39.
    // Let's use col 0 — it fits exactly in 40 columns.
    // ------------------------------------------------------------------
    VDP_drawText("C O N W A Y ' S  G A M E  O F  L I F E", 0, 4);

    // Repeat the title one row lower with surrounding dashes for weight,
    // keeping the same width (39 chars, col 0).
    VDP_drawText("---------------------------------------", 0, 5);

    // ------------------------------------------------------------------
    // Controls block
    // Header centred: "Controls:" = 9 chars → col (40-9)/2 = 15
    // ------------------------------------------------------------------
    VDP_drawText("Controls:", 15, 8);

    // Each control line — longest is 34 chars, centred → col (40-34)/2 = 3
    VDP_drawText("A / B / C  -  Toggle cell on or off", 2, 10);
    //           "  D-Pad   -  Move the cursor       " = 34 chars
    VDP_drawText("  D-Pad   -  Move the cursor      ", 3, 11);
    //           "  Start   -  Run / Pause           " = 34 chars
    VDP_drawText("  Start   -  Run / Pause          ", 3, 12);

    // ------------------------------------------------------------------
    // Press Start prompt — centred at row 21
    // "- Press Start to Begin -" = 24 chars → col (40-24)/2 = 8
    // ------------------------------------------------------------------
    VDP_drawText("- Press Start to Begin -", 8, 21);
}

// ---------------------------------------------------------------------------
// Draw a single cell at (x, y) according to current grid + cursor state
// ---------------------------------------------------------------------------
static void draw_cell(u16 x, u16 y)
{
    u16 attr;

    // If paused and this is the cursor position, draw the appropriate cursor tile
    if (!running && x == cursor_x && y == cursor_y)
    {
        attr = grid[x][y] ? ATTR_CURSOR_FILLED : ATTR_CURSOR_EMPTY;
    }
    else if (grid[x][y])
    {
        attr = ATTR_LIVE;
    }
    else
    {
        attr = ATTR_DEAD;
    }

    VDP_setTileMapXY(BG_B, attr, x, y);
}

// ---------------------------------------------------------------------------
// Redraw the entire grid
// ---------------------------------------------------------------------------
static void draw_all(void)
{
    u16 x, y;
    for (y = 0; y < GRID_H; y++)
        for (x = 0; x < GRID_W; x++)
            draw_cell(x, y);
}

// ---------------------------------------------------------------------------
// Count live neighbours of cell (x, y) – edges are hard boundaries (no wrap)
// ---------------------------------------------------------------------------
static u8 count_neighbours(u16 x, u16 y)
{
    u8  count = 0;
    s16 nx, ny;

    for (ny = (s16)y - 1; ny <= (s16)y + 1; ny++)
    {
        if (ny < 0 || ny >= GRID_H) continue;
        for (nx = (s16)x - 1; nx <= (s16)x + 1; nx++)
        {
            if (nx < 0 || nx >= GRID_W) continue;
            if (nx == (s16)x && ny == (s16)y) continue;
            count += grid[nx][ny];
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Advance one Conway generation into 'next', then swap buffers
// ---------------------------------------------------------------------------
static void step_simulation(void)
{
    u16 x, y;
    u8  n;

    for (y = 0; y < GRID_H; y++)
    {
        for (x = 0; x < GRID_W; x++)
        {
            n = count_neighbours(x, y);
            if (grid[x][y])
                next[x][y] = (n == 2 || n == 3) ? 1 : 0;
            else
                next[x][y] = (n == 3) ? 1 : 0;
        }
    }

    // Swap buffers
    for (y = 0; y < GRID_H; y++)
        for (x = 0; x < GRID_W; x++)
            grid[x][y] = next[x][y];
}

// ---------------------------------------------------------------------------
// How many frames to wait between simulation steps when running.
// At 60 fps this gives roughly 10 generations per second – comfortable to watch.
// ---------------------------------------------------------------------------
#define SIM_FRAME_DELAY 6

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(u16 hard)
{
    u16 x, y;
    u16 prev_cursor_x, prev_cursor_y;
    u8  sim_timer = 0;

    // -----------------------------------------------------------------------
    // VDP / palette setup
    // -----------------------------------------------------------------------
    VDP_clearPlane(BG_A, TRUE);
    VDP_clearPlane(BG_B, TRUE);

    // Palette 0:
    //   Entry 0 – light grey backdrop (global background colour)
    //   Entry 1 – darker grey  (dead cell / grid)
    //   Entry 2 – yellow       (live cell)
    //   Entry 3 – blue         (cursor border)
    //
    // Genesis VDP colour format: 0x0BGR  (4 bits per channel)
    //   Light grey:  R=0xA, G=0xA, B=0xA  → 0x0AAA
    //   Darker grey: R=0x6, G=0x6, B=0x6  → 0x0666
    //   Yellow:      R=0xE, G=0xE, B=0x0  → 0x00EE  (B=0, G=E, R=E)
    //   Blue:        R=0x0, G=0x0, B=0xE  → 0x0E00
    PAL_setColor(0,  0x0AAA);  // entry 0 – light grey backdrop
    PAL_setColor(1,  0x0666);  // entry 1 – dark grey dead cell
    PAL_setColor(2,  0x00EE);  // entry 2 – yellow live cell
    PAL_setColor(3,  0x0E00);  // entry 3 – blue cursor border

    // -----------------------------------------------------------------------
    // Load our three solid-colour tiles into VRAM
    // VDP_loadTileData(data, vram_index, num_tiles, use_dma)
    // -----------------------------------------------------------------------
    VDP_loadTileData(tile_dead,          TILE_IDX_DEAD,          1, CPU);
    VDP_loadTileData(tile_live,          TILE_IDX_LIVE,          1, CPU);
    VDP_loadTileData(tile_cursor_empty,  TILE_IDX_CURSOR_EMPTY,  1, CPU);
    VDP_loadTileData(tile_cursor_filled, TILE_IDX_CURSOR_FILLED, 1, CPU);

    // -----------------------------------------------------------------------
    // Initialise game state
    // -----------------------------------------------------------------------
    for (y = 0; y < GRID_H; y++)
        for (x = 0; x < GRID_W; x++)
            grid[x][y] = 0;

    cursor_x = GRID_W / 2;
    cursor_y = GRID_H / 2;
    running  = 0;
    in_menu  = 1;

    flag_start       = 0;
    flag_toggle_cell = 0;
    flag_move_left   = 0;
    flag_move_right  = 0;
    flag_move_up     = 0;
    flag_move_down   = 0;

    // -----------------------------------------------------------------------
    // Input
    // -----------------------------------------------------------------------
    JOY_init();
    JOY_setEventHandler(&joy_handler);

    // -----------------------------------------------------------------------
    // Initial draw – fill BG_B with dead cells, then overlay the menu on BG_A
    // -----------------------------------------------------------------------
    draw_all();
    draw_menu();

    // -----------------------------------------------------------------------
    // Main game loop
    // -----------------------------------------------------------------------
    while (TRUE)
    {
        // --- Handle input flags (set by joypad callback) -------------------

        if (flag_start)
        {
            flag_start = 0;

            if (in_menu)
            {
                // First Start press: dismiss the menu permanently and enter the game
                in_menu = 0;
                VDP_clearPlane(BG_A, TRUE);   // wipe all menu text; game runs on BG_B

                // Hydrate grid from SRAM if a valid save exists
                sram_load();

                // Repaint full grid (may have changed after SRAM load)
                draw_all();
                draw_cell(cursor_x, cursor_y); // show cursor on the now-visible grid
            }
            else
            {
                // Subsequent Start presses: toggle simulation running state
                running = !running;

                if (running)
                {
                    // Hide cursor: redraw cell under the old cursor position
                    draw_cell(cursor_x, cursor_y);
                    sim_timer = 0;
                }
                else
                {
                    // Show cursor at current position
                    draw_cell(cursor_x, cursor_y);

                    // Persist grid to SRAM whenever the player pauses
                    sram_save();
                }
            }
        }

        if (!in_menu)
        {
            if (!running)
            {
                // Remember where cursor was so we can erase it after movement
                prev_cursor_x = cursor_x;
                prev_cursor_y = cursor_y;

                if (flag_move_left)
                {
                    flag_move_left = 0;
                    if (cursor_x > 0) cursor_x--;
                }
                if (flag_move_right)
                {
                    flag_move_right = 0;
                    if (cursor_x < GRID_W - 1) cursor_x++;
                }
                if (flag_move_up)
                {
                    flag_move_up = 0;
                    if (cursor_y > 0) cursor_y--;
                }
                if (flag_move_down)
                {
                    flag_move_down = 0;
                    if (cursor_y < GRID_H - 1) cursor_y++;
                }

                // If cursor moved, redraw old cell (no longer cursor) and new cell
                if (prev_cursor_x != cursor_x || prev_cursor_y != cursor_y)
                {
                    draw_cell(prev_cursor_x, prev_cursor_y);
                    draw_cell(cursor_x, cursor_y);
                }

                // Toggle cell under cursor
                if (flag_toggle_cell)
                {
                    flag_toggle_cell = 0;
                    grid[cursor_x][cursor_y] ^= 1;
                    draw_cell(cursor_x, cursor_y);

                    // Persist grid to SRAM after each edit
                    sram_save();
                }
            }
            else
            {
                // Discard any stale movement/toggle flags accumulated while running
                flag_move_left   = 0;
                flag_move_right  = 0;
                flag_move_up     = 0;
                flag_move_down   = 0;
                flag_toggle_cell = 0;

                // Advance simulation every SIM_FRAME_DELAY frames
                sim_timer++;
                if (sim_timer >= SIM_FRAME_DELAY)
                {
                    sim_timer = 0;
                    step_simulation();
                    draw_all();
                }
            }
        } // end !in_menu

        // --- End of frame: VBlank sync + flush DMA/sprite/scroll queues ---
        SYS_doVBlankProcess();
    }

    return 0;
}