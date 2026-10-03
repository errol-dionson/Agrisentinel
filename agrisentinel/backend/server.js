import express from "express";
import dotenv from "dotenv";

dotenv.config();

const app = express();
const PORT = Number(process.env.PORT || 3001);
const OPENROUTER_MODEL = process.env.OPENROUTER_MODEL || "google/gemini-2.5-flash";
const OPENROUTER_API_KEY = process.env.OPENROUTER_API_KEY || "";

app.use((req, res, next) => {
  const allowed = (process.env.ALLOWED_ORIGINS || "http://localhost:3000").split(",");
  const origin = req.headers.origin;

  if (origin && allowed.includes(origin)) {
    res.setHeader("Access-Control-Allow-Origin", origin);
  }

  res.setHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  res.setHeader("Access-Control-Allow-Headers", "Content-Type,Authorization");

  if (req.method === "OPTIONS") {
    res.sendStatus(204);
    return;
  }

  next();
});

app.get("/api/health", (req, res) => {
  res.json({
    success: true,
    status: "ok",
    model: OPENROUTER_MODEL,
    backend: "ready",
    aiConfigured: Boolean(OPENROUTER_API_KEY),
  });
});

function parseNormalizedJson(rawText) {
  if (!rawText || typeof rawText !== "string") {
    return null;
  }

  let cleaned = rawText.trim();
  const fenced = cleaned.match(/```json\s*([\s\S]*?)\s*```/i);
  if (fenced && fenced[1]) {
    cleaned = fenced[1].trim();
  }

  try {
    return JSON.parse(cleaned);
  } catch {
    const firstBrace = cleaned.indexOf("{");
    const lastBrace = cleaned.lastIndexOf("}");
    if (firstBrace >= 0 && lastBrace > firstBrace) {
      try {
        return JSON.parse(cleaned.substring(firstBrace, lastBrace + 1));
      } catch {
        return null;
      }
    }
    return null;
  }
}

app.post("/api/analyze-bird", express.raw({ type: "image/jpeg", limit: "10mb" }), async (req, res) => {
  try {
    if (!Buffer.isBuffer(req.body) || req.body.length === 0) {
      return res.status(400).json({ success: false, error: "Empty image payload" });
    }

    if (!OPENROUTER_API_KEY) {
      return res.status(503).json({
        success: false,
        bird_detected: false,
        confidence: 0,
        bird_type: "unknown",
        description: "OpenRouter API key is not configured.",
        recommended_action: "monitor",
      });
    }

    const base64Image = req.body.toString("base64");
    const prompt = `You are a bird detection validator for a crop protection system. Analyze the supplied image and determine whether a bird is present. Return ONLY valid JSON with exactly these keys: bird_detected, confidence, bird_type, description, recommended_action. Value rules: bird_detected is true when a bird is present with reasonable certainty, confidence is between 0 and 1, bird_type should be a short label such as "sparrow", "pigeon", "hawk", or "unknown" if uncertain, description should be brief and factual, recommended_action should be "deter" when action is appropriate or "monitor" when it is not. Do not claim an exact species if the image is insufficient.`;

    const response = await fetch("https://openrouter.ai/api/v1/chat/completions", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${OPENROUTER_API_KEY}`,
        "HTTP-Referer": process.env.BACKEND_HOST || "http://localhost:3001",
        "X-Title": "Agrisentinel Bird Detector",
      },
      body: JSON.stringify({
        model: OPENROUTER_MODEL,
        messages: [
          {
            role: "user",
            content: [
              { type: "text", text: prompt },
              {
                type: "image_url",
                image_url: {
                  url: `data:image/jpeg;base64,${base64Image}`,
                },
              },
            ],
          },
        ],
        temperature: 0.2,
      }),
    });

    const rawText = await response.text();
    if (!response.ok) {
      return res.status(502).json({
        success: false,
        bird_detected: false,
        confidence: 0,
        bird_type: "unknown",
        description: `OpenRouter request failed (${response.status}): ${rawText.slice(0, 200)}`,
        recommended_action: "monitor",
      });
    }

    let parsed;
    try {
      parsed = JSON.parse(rawText);
    } catch {
      return res.status(502).json({
        success: false,
        bird_detected: false,
        confidence: 0,
        bird_type: "unknown",
        description: "OpenRouter returned invalid JSON.",
        recommended_action: "monitor",
      });
    }

    const messageContent = parsed?.choices?.[0]?.message?.content ?? "";
    const jsonPayload = parseNormalizedJson(messageContent);

    if (!jsonPayload) {
      return res.status(502).json({
        success: false,
        bird_detected: false,
        confidence: 0,
        bird_type: "unknown",
        description: "OpenRouter response was not valid structured JSON.",
        recommended_action: "monitor",
      });
    }

    const confidence = Number(jsonPayload.confidence ?? 0);
    const birdDetected = Boolean(jsonPayload.bird_detected === true && Number.isFinite(confidence) && confidence >= 0 && confidence <= 1);

    const safePayload = {
      success: true,
      bird_detected: birdDetected,
      confidence: Number.isFinite(confidence) ? Math.min(Math.max(confidence, 0), 1) : 0,
      bird_type: typeof jsonPayload.bird_type === "string" ? jsonPayload.bird_type : "unknown",
      description: typeof jsonPayload.description === "string" ? jsonPayload.description : "Bird status could not be determined.",
      recommended_action: typeof jsonPayload.recommended_action === "string" ? jsonPayload.recommended_action : "monitor",
    };

    return res.json(safePayload);
  } catch (error) {
    console.error("[backend] analyze-bird failed:", error);
    return res.status(500).json({
      success: false,
      bird_detected: false,
      confidence: 0,
      bird_type: "unknown",
      description: "Backend analysis failed.",
      recommended_action: "monitor",
    });
  }
});

app.listen(PORT, () => {
  console.log(`Agrisentinel backend listening on http://localhost:${PORT}`);
  console.log(`OpenRouter model: ${OPENROUTER_MODEL}`);
});
