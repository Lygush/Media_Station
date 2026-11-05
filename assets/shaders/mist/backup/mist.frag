#version 440 core

layout(location = 0) in  vec2 coord;
layout(location = 0) out vec4 fragColor;

// Один UBO — тот же что в .vert, Qt добавляет свои поля первыми,
// наши кастомные поля идут строго после qt_Opacity.
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

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);          // smoothstep

    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));

    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Fractional Brownian Motion — суммируем несколько октав шума
float fbm(vec2 p) {
    float v   = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    for (int i = 0; i < 6; i++) {
        v   += amp  * noise(p * freq);
        amp  *= 0.5;
        freq *= 2.0;
        // лёгкое вращение между октавами — убирает видимые артефакты
        p = mat2(0.8, -0.6, 0.6, 0.8) * p;
    }
    return v;
}

void main() {
    vec2 uv = coord;
    float t = time * speed;

    // ── Высотная маска: струи только в нижней половине ───────────────────────
    float heightMask = clamp((uv.y - 0.35) / 0.65, 0.0, 1.0);
    heightMask = pow(heightMask, 1.2);

    // ── СТРУИ: горизонтально вытянутые полосы на разных высотах ─────────────
    // Каждая струя — синус по X с FBM-возмущением, на своей высоте
    // uv.y растёт вниз, поэтому высоты 0.55..0.95

    float streams = 0.0;

    // Струя 1 — широкая, у самого дна
    {
        float bandY  = 0.82;
        float dy     = uv.y - bandY;
        float noise1 = fbm(vec2(uv.x * scale * 0.8 + t * 0.18, t * 0.05));
        float noise2 = fbm(vec2(uv.x * scale * 1.2 - t * 0.12, t * 0.07 + 2.0));
        float warp   = (noise1 - 0.5) * 0.12; // волнение по вертикали
        float dist   = abs(dy + warp);
        float width  = 0.055 + noise2 * 0.04;
        float s = smoothstep(width, 0.0, dist);
        float density = fbm(vec2(uv.x * scale * 2.0 + t * 0.22, bandY * scale));
        streams += s * density * 1.15;
    }

    // Струя 2 — средняя высота, движется влево
    {
        float bandY  = 0.68;
        float dy     = uv.y - bandY;
        float noise1 = fbm(vec2(uv.x * scale * 0.6 - t * 0.14, t * 0.04 + 1.3));
        float noise2 = fbm(vec2(uv.x * scale * 1.5 + t * 0.10, t * 0.06 + 3.7));
        float warp   = (noise1 - 0.5) * 0.10;
        float dist   = abs(dy + warp);
        float width  = 0.040 + noise2 * 0.03;
        float s = smoothstep(width, 0.0, dist);
        float density = fbm(vec2(uv.x * scale * 1.8 - t * 0.16, bandY * scale));
        streams += s * density * 0.88;
    }

    // Струя 3 — тонкая, повыше, медленная
    {
        float bandY  = 0.56;
        float dy     = uv.y - bandY;
        float noise1 = fbm(vec2(uv.x * scale * 0.4 + t * 0.08, t * 0.03 + 2.5));
        float noise2 = fbm(vec2(uv.x * scale * 0.9 - t * 0.07, t * 0.04 + 5.1));
        float warp   = (noise1 - 0.5) * 0.08;
        float dist   = abs(dy + warp);
        float width  = 0.025 + noise2 * 0.02;
        float s = smoothstep(width, 0.0, dist);
        float density = fbm(vec2(uv.x * scale * 1.4 + t * 0.09, bandY * scale));
        streams += s * density * 0.50;
    }

    // ── ТОНКИЕ СТРУЙКИ ───────────────────────────────────────────────────────

    // Тонкая струйка A
    {
        float bandY  = 0.76;
        float dy     = uv.y - bandY;
        float noise1 = fbm(vec2(uv.x * scale * 1.8 + t * 0.55, t * 0.08 + 1.1));
        float warp   = (noise1 - 0.5) * 0.05;
        float dist   = abs(dy + warp);
        float s = smoothstep(0.012, 0.0, dist);
        float density = fbm(vec2(uv.x * scale * 3.5 + t * 0.60, bandY * scale * 2.0));
        density = smoothstep(0.42, 0.65, density);
        streams += s * density * 0.22;  // было 0.55 — теперь тихо
    }

    // Тонкая струйка B
    {
        float bandY  = 0.62;
        float dy     = uv.y - bandY;
        float noise1 = fbm(vec2(uv.x * scale * 2.2 - t * 0.42, t * 0.06 + 3.8));
        float warp   = (noise1 - 0.5) * 0.04;
        float dist   = abs(dy + warp);
        float s = smoothstep(0.010, 0.0, dist);
        float density = fbm(vec2(uv.x * scale * 4.0 - t * 0.48, bandY * scale * 2.0));
        density = smoothstep(0.44, 0.64, density);
        streams += s * density * 0.16;  // было 0.40
    }

    // Тонкая струйка C
    {
        float bandY  = 0.50;
        float dy     = uv.y - bandY;
        float noise1 = fbm(vec2(uv.x * scale * 1.5 + t * 0.70, t * 0.05 + 6.3));
        float warp   = (noise1 - 0.5) * 0.03;
        float dist   = abs(dy + warp);
        float s = smoothstep(0.008, 0.0, dist);
        float density = fbm(vec2(uv.x * scale * 5.0 + t * 0.75, bandY * scale * 2.0));
        density = smoothstep(0.46, 0.62, density);
        streams += s * density * 0.10;  // было 0.25
    }
    {
        float bandY  = 0.74;
        float dy     = uv.y - bandY;
        float noise1 = fbm(vec2(uv.x * scale * 0.5 - t * 0.09, t * 0.035 + 4.2));
        float warp   = (noise1 - 0.5) * 0.14;
        float dist   = abs(dy + warp);
        float width  = 0.080;
        float s = smoothstep(width, 0.0, dist);
        streams += s * 0.30;
    }

    streams = clamp(streams, 0.0, 1.0);

    // ── Горизонтальный базовый слой снизу ───────────────────────────────────
    // Плотный у дна, плавно затухает кверху — без конусов и холмов
    float baseNoise = fbm(vec2(uv.x * scale * 0.8 + t * 0.18, uv.y * scale * 0.4));
    float baseMask  = clamp((uv.y - 0.78) / 0.22, 0.0, 1.0);
    baseMask = pow(baseMask, 0.5);
    float base = baseMask * smoothstep(0.28, 0.62, baseNoise);
    base = clamp(base, 0.0, 1.0);

    float intensity = streams * heightMask + base * 0.60;
    intensity = clamp(intensity, 0.0, 1.0);

    // ── Цвет: почти белый с холодным оттенком ───────────────────────────────
    // Нижние струи чуть теплее, верхние — прохладнее
    vec3 colWarm = vec3(0.95, 0.97, 1.00);
    vec3 colCool = vec3(0.80, 0.88, 1.00);
    vec3 col = mix(colCool, colWarm, heightMask);

    float alpha = intensity * 0.48 * qt_Opacity;

    fragColor = vec4(col * alpha, alpha);
}
