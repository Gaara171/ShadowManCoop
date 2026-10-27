// D3D11-Overlay: Namensschilder und Pfeile der Mitspieler in der Spielschrift "FE" (aus ShadowManEX01.kpf).
// Gezeichnet wird ueber RenderFrame aus der Present-Funktion des Spiels, kurz vor dem eigentlichen Present.
#pragma once
#include <cstdint>

namespace overlay {

struct Label {
    float x = 0, y = 0;      // Mitte (align 0) bzw. linker Rand (align 1), oben; Bildschirm -1..1, y nach oben
    float scale = 1.0f;      // 1 = Grundgroesse (etwa 4 % der Bildhoehe)
    uint32_t rgb = 0xFFFFFF;
    float alpha = 1.0f;
    int align = 0;
    char text[96] = {};      // Latin-1
};

struct Arrow {
    float x = 0, y = 0;      // Spitze, Bildschirm -1..1
    float angle = 0;         // Richtung in Radiant (0 = rechts, -pi/2 = nach unten)
    float size = 1.0f;       // 1 = Grundgroesse
    uint32_t rgb = 0xFFFFFF;
    float alpha = 1.0f;
};

struct Frame {
    Label labels[24];
    int numLabels = 0;
    Arrow arrows[8];
    int numArrows = 0;
};

using FrameFn = void (*)(Frame& frame, float screenW, float screenH);  // im Render-Thread, je Bild
using LogFn = void (*)(const char* fmt, ...);

// kpfPath: ShadowManEX01.kpf (Schrift); liefert false, wenn die Schrift fehlt
bool Install(const char* kpfPath, FrameFn frameFn, LogFn log);

// im Render-Thread je Bild vor dem Present des Spiels (swapChain = IDXGISwapChain* des Spiels)
void RenderFrame(void* swapChain);

}  // namespace overlay
