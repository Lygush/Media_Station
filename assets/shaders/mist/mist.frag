#version 440 core

layout(location = 0) in  vec2 coord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    float time;
    float scale;
    float speed;
};

// ── Хэш / шум ───────────────────────────────────────────────────────────────
float hash(vec2 p) {
    p = fract(p * vec2(127.1, 311.7));
    p += dot(p, p + 19.19);
    return fract(p.x * p.y);
}

// Номер клетки заворачивается по модулю 289: аргумент hash() остаётся малым,
// и через часы работы (время растёт) узор не начинает «рябить квадратами»
// из-за потери точности float. Соседи берутся тоже по модулю — без шва.
float noise(vec2 p) {
    vec2 i  = mod(floor(p), 289.0);
    vec2 i1 = mod(i + 1.0, 289.0);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(vec2(i1.x, i.y));
    float c = hash(vec2(i.x, i1.y));
    float d = hash(i1);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
    float v   = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    for (int i = 0; i < 6; i++) {
        v   += amp  * noise(p * freq);
        amp  *= 0.5;
        freq *= 2.0;
        p = mat2(0.8, -0.6, 0.6, 0.8) * p;
    }
    return v;
}

void main() {
    vec2 uv = coord;

    // Верхние 36 % экрана всегда прозрачны (маска высоты и базовый слой там
    // равны нулю) — не считаем там ~130 вычислений шума на пиксель.
    if (uv.y < 0.36) {
        fragColor = vec4(0.0);
        return;
    }

    float t = time * speed;

    // ── Высотная маска: струи только в нижней половине ───────────────────────
    float heightMask = smoothstep(0.36, 0.84, uv.y);
    heightMask = pow(heightMask, 1.25);

    // ── СТРУИ ───────────────────────────────────────────────────────────────
    float streams = 0.0;

    // Струя 1 — широкая, у дна
    {
        float bandY  = 0.82;
        float dy     = uv.y - bandY;
        float noiseX = fbm(vec2(uv.x * scale * 0.8 + t * 0.18, t * 0.05));
        float noiseY = fbm(vec2(uv.x * scale * 0.7 - t * 0.23, t * 0.13 + 1.7));
        float noise2 = fbm(vec2(uv.x * scale * 1.2 - t * 0.12, t * 0.07 + 2.0));
        float warp   = (noiseX - 0.5) * 0.10 + (noiseY - 0.5) * 0.08;
        float dist   = abs(dy + warp);
        float width  = 0.055 + noise2 * 0.04;
        float sigma  = width * 0.55;
        float s = exp(- (dist * dist) / (2.0 * sigma * sigma));
        float density = fbm(vec2(uv.x * scale * 2.0 + t * 0.22, bandY * scale));
        streams += s * density * 0.95;
    }

    // Струя 2 — средняя (особо мягкая)
    {
        float bandY  = 0.68;
        float dy     = uv.y - bandY;
        float noise1 = fbm(vec2(uv.x * scale * 0.6 - t * 0.14, t * 0.04 + 1.3));
        float noise2 = fbm(vec2(uv.x * scale * 1.5 + t * 0.10, t * 0.06 + 3.7));
        float warp   = (noise1 - 0.5) * 0.10;
        float dist   = abs(dy + warp);
        float width  = 0.040 + noise2 * 0.03;
        float sigma  = width * 0.7;
        float s = exp(- (dist * dist) / (2.0 * sigma * sigma));
        float density = fbm(vec2(uv.x * scale * 1.8 - t * 0.16, bandY * scale));
        streams += s * density * 0.88;
    }

    // Струя 3 — тонкая, повыше
    {
        float bandY  = 0.56;
        float dy     = uv.y - bandY;
        float noise1 = fbm(vec2(uv.x * scale * 0.4 + t * 0.08, t * 0.03 + 2.5));
        float noise2 = fbm(vec2(uv.x * scale * 0.9 - t * 0.07, t * 0.04 + 5.1));
        float warp   = (noise1 - 0.5) * 0.08;
        float dist   = abs(dy + warp);
        float width  = 0.025 + noise2 * 0.02;
        float sigma  = width * 0.6;
        float s = exp(- (dist * dist) / (2.0 * sigma * sigma));
        float density = fbm(vec2(uv.x * scale * 1.4 + t * 0.09, bandY * scale));
        streams += s * density * 0.50;
    }

    // Тонкая струйка A
    {
        float bandY  = 0.76;
        float dy     = uv.y - bandY;
        float noise1 = fbm(vec2(uv.x * scale * 1.8 + t * 0.55, t * 0.08 + 1.1));
        float warp   = (noise1 - 0.5) * 0.05;
        float dist   = abs(dy + warp);
        float sigma  = 0.006;
        float s = exp(- (dist * dist) / (2.0 * sigma * sigma));
        float density = fbm(vec2(uv.x * scale * 3.5 + t * 0.60, bandY * scale * 2.0));
        density = smoothstep(0.42, 0.65, density);
        streams += s * density * 0.22;
    }

    // Тонкая струйка B
    {
        float bandY  = 0.62;
        float dy     = uv.y - bandY;
        float noise1 = fbm(vec2(uv.x * scale * 2.2 - t * 0.42, t * 0.06 + 3.8));
        float warp   = (noise1 - 0.5) * 0.04;
        float dist   = abs(dy + warp);
        float sigma  = 0.005;
        float s = exp(- (dist * dist) / (2.0 * sigma * sigma));
        float density = fbm(vec2(uv.x * scale * 4.0 - t * 0.48, bandY * scale * 2.0));
        density = smoothstep(0.44, 0.64, density);
        streams += s * density * 0.16;
    }

    // Тонкая струйка C
    {
        float bandY  = 0.50;
        float dy     = uv.y - bandY;
        float noise1 = fbm(vec2(uv.x * scale * 1.5 + t * 0.70, t * 0.05 + 6.3));
        float warp   = (noise1 - 0.5) * 0.03;
        float dist   = abs(dy + warp);
        float sigma  = 0.004;
        float s = exp(- (dist * dist) / (2.0 * sigma * sigma));
        float density = fbm(vec2(uv.x * scale * 5.0 + t * 0.75, bandY * scale * 2.0));
        density = smoothstep(0.46, 0.62, density);
        streams += s * density * 0.10;
    }

    // Дополнительная широкая полоса
    {
        float bandY  = 0.74;
        float dy     = uv.y - bandY;
        float noise1 = fbm(vec2(uv.x * scale * 0.5 - t * 0.09, t * 0.035 + 4.2));
        float warp   = (noise1 - 0.5) * 0.14;
        float dist   = abs(dy + warp);
        float width  = 0.080;
        float sigma  = width * 0.65;
        float s = exp(- (dist * dist) / (2.0 * sigma * sigma));
        streams += s * 0.30;
    }

    streams = clamp(streams, 0.0, 1.0);

    // ── Горизонтальный базовый слой снизу (хаотичный) ──────────────────────
    float vertShift = fbm(vec2(uv.x * 0.4 + t * 0.07, t * 0.17)) * 0.18;
    float baseA = fbm(vec2(uv.x * scale * 0.9 + t * 0.12, (uv.y + vertShift) * scale * 0.4));
    float baseB = fbm(vec2(uv.x * scale * 1.3 - t * 0.23, uv.y * scale * 0.55 - t * 0.09 + 3.0));
    float baseC = fbm(vec2(uv.x * scale * 0.6 + t * 0.31, uv.y * scale * 0.3 + t * 0.14 + 5.5));
    float baseNoise = baseA * 0.55 + baseB * 0.3 + baseC * 0.15;

    float baseMask  = smoothstep(0.78, 0.98, uv.y + fbm(vec2(uv.x * scale * 0.42 - t * 0.06, uv.y * scale * 0.18 + 7.1)) * 0.06);
    float base = baseMask * smoothstep(0.30, 0.62, baseNoise);
    base = clamp(base, 0.0, 1.0);

    float intensity = streams * heightMask + base * 0.28;
    intensity = clamp(intensity, 0.0, 1.0);

    // ── Цвет ────────────────────────────────────────────────────────────────
    vec3 colWarm = vec3(0.95, 0.97, 1.00);
    vec3 colCool = vec3(0.80, 0.88, 1.00);
    vec3 col = mix(colCool, colWarm, heightMask);
    float luminance = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(vec3(luminance), col, 1.08);

    float alpha = intensity * 0.38 * qt_Opacity;

    fragColor = vec4(col * alpha, alpha);
}