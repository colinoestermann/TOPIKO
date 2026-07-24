#pragma once
// TOPIKO Handheld — Typen fuer den Blob-Renderer.
// Eigener Header, damit die Arduino-Prototypen-Generierung den Typ kennt.

struct BlobStyle {
  float r1, g1, b1;     // Kernfarbe (heller Lichtpunkt oben)
  float r2, g2, b2;     // Mittelfarbe (Koerper)
  float r3, g3, b3;     // Randfarbe (aeusserer Glow)
  float beatPeriod;     // Herzschlag-Periode in Sekunden (Lub-dub)
  float beatAmp;        // Ausschlag des Herzschlags
  float audioReact;     // wie stark der Raumklang die Form treibt
  float wobble;         // organische Unruhe der Kontur
  float radius;         // Grundradius in Renderpixeln
};
