#pragma once

// ER-301 panel layout in millimetres.
//
// Physical ER-301 panel registration in millimetres.
// Keep geometry changes deliberate: these coordinates track the hardware faceplate.
namespace er301layout {
static constexpr float PANEL_W = 152.40f;
static constexpr float PANEL_H = 128.50f;

struct XY {
    float x;
    float y;
};

static constexpr XY SCREW[4] = {
    {7.45f, 3.25f}, {144.95f, 3.25f},
    {7.45f, 125.25f}, {144.95f, 125.25f}};

static constexpr XY KNOB = {23.90f, 73.30f};
static constexpr float KNOB_DIAMETER = 28.20f;

static constexpr XY M[6] = {
    {9.60f, 49.10f}, {23.90f, 49.10f}, {38.00f, 49.10f},
    {52.40f, 49.10f}, {66.50f, 49.10f}, {80.60f, 49.10f}};

static constexpr XY DIAL[3] = {
    {9.60f, 97.00f}, {23.90f, 97.00f}, {38.00f, 97.00f}};
static constexpr XY S[3] = {
    {52.40f, 97.00f}, {66.50f, 97.00f}, {80.60f, 97.00f}};
static constexpr XY HB[3] = {
    {52.40f, 113.10f}, {66.50f, 113.10f}, {80.60f, 113.10f}};

static constexpr XY SELECT[4] = {
    {101.10f, 17.30f}, {101.10f, 33.20f},
    {101.10f, 49.10f}, {101.10f, 65.10f}};
static constexpr XY LINK[3] = {
    {96.00f, 25.25f}, {96.00f, 41.20f}, {96.00f, 57.10f}};

static constexpr XY G[4] = {
    {115.30f, 17.30f}, {115.30f, 33.20f},
    {115.30f, 49.10f}, {115.30f, 65.10f}};
static constexpr XY IN[4] = {
    {129.40f, 17.30f}, {129.40f, 33.20f},
    {129.40f, 49.10f}, {129.40f, 65.10f}};
static constexpr XY OUT[4] = {
    {143.70f, 17.30f}, {143.70f, 33.20f},
    {143.70f, 49.10f}, {143.70f, 65.10f}};

static constexpr XY A[3] = {
    {101.10f, 81.20f}, {101.10f, 97.10f}, {101.10f, 113.10f}};
static constexpr XY B[3] = {
    {115.30f, 81.20f}, {115.30f, 97.10f}, {115.30f, 113.10f}};
static constexpr XY C[3] = {
    {129.40f, 81.20f}, {129.40f, 97.10f}, {129.40f, 113.10f}};
static constexpr XY D[3] = {
    {143.70f, 81.20f}, {143.70f, 97.10f}, {143.70f, 113.10f}};

static constexpr XY LED_OUT[4] = {
    {91.60f, 17.30f}, {91.60f, 33.20f},
    {91.60f, 49.10f}, {91.60f, 65.10f}};
static constexpr XY LED_LINK[3] = {
    {91.60f, 25.25f}, {91.60f, 41.20f}, {91.60f, 57.10f}};

// The ABCD indicators are not tucked immediately beside the jacks.  Their
// centres sit roughly 7.2 mm to the left and 3.9 mm above each jack centre.
static constexpr XY ABCD_LED[4][3] = {
    {{93.90f, 77.25f}, {93.90f, 93.15f}, {93.90f, 109.15f}},
    {{108.10f, 77.25f}, {108.10f, 93.15f}, {108.10f, 109.15f}},
    {{122.20f, 77.25f}, {122.20f, 93.15f}, {122.20f, 109.15f}},
    {{136.50f, 77.25f}, {136.50f, 93.15f}, {136.50f, 109.15f}}};

static constexpr XY TOG_STORAGE = {7.75f, 113.00f};
static constexpr XY TOG_MODE = {36.70f, 113.00f};
static constexpr XY LED_FINE = {9.20f, 84.45f};
static constexpr XY LED_COARSE = {14.65f, 89.00f};
static constexpr XY LED_IO = {27.20f, 109.70f};
static constexpr XY LED_SAFE = {27.20f, 115.85f};

// x, y, width, height of the live framebuffer apertures.
static constexpr float MAIN_DISP[4] = {6.95f, 15.95f, 76.40f, 18.85f};
static constexpr float SUB_DISP[4] = {49.10f, 64.65f, 34.80f, 17.10f};
}
