import React, { useState, useEffect, useCallback, useRef } from "react";
import emailjs from "@emailjs/browser";
import {
  AreaChart,
  Area,
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  ResponsiveContainer,
  ReferenceLine,
} from "recharts";

// ============================================================
// IoT Supply Chain Tracking Dashboard
// ============================================================
// CONFIGURATION — paste your credentials here:
//   1. Open this file
//   2. Replace CHANNEL_ID and READ_API_KEY below
// ============================================================
const THINGSPEAK_CONFIG = {
  CHANNEL_ID: "3399994",
  READ_API_KEY: "BREHPT7JI3T1ZIWP",
  RESULTS: 30,
  POLL_INTERVAL_MS: 20000,
};

// Change this value to set the dashboard login password.
const DASHBOARD_PASSWORD = "123456";

// ============================================================
// EMAIL ALERTS — EmailJS configuration
// Sender: pranavadiga55@gmail.com (connect this Gmail in EmailJS)
// Receiver: kiransk5855@gmail.com
// Sign up at https://www.emailjs.com and fill in the values below.
// ============================================================
const EMAILJS_CONFIG = {
  SERVICE_ID: "service_uv6g7wm",      // EmailJS → Email Services → Service ID
  TEMPLATE_ID: "template_x9jpube",    // EmailJS → Email Templates → Template ID
  PUBLIC_KEY: "NNDPG_cYWOvlKCm07",       // EmailJS → Account → API Keys → Public Key
  TO_EMAIL: "ksk5842hh@gmail.com",  // recipient email address
  COOLDOWN_MS: 5 * 60 * 1000,         // 5 minutes between emails
};



const safeFloat = (val, decimals = 2) => {
  const n = parseFloat(val);
  return isNaN(n) ? null : parseFloat(n.toFixed(decimals));
};

const fmt = (val, unit = "", decimals = 1) => {
  if (val === null || val === undefined) return "--";
  return `${safeFloat(val, decimals) ?? "--"}${unit}`;
};

const fmtCoordinate = (val) =>
  Number.isFinite(val) ? val.toFixed(6) : "--";

const fmtTime = (iso) => {
  try {
    const d = new Date(iso);
    return d.toLocaleTimeString([], {
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit",
    });
  } catch {
    return "--";
  }
};

const fetchThingSpeakData = async () => {
  const url = `https://api.thingspeak.com/channels/${THINGSPEAK_CONFIG.CHANNEL_ID}/feeds.json?api_key=${THINGSPEAK_CONFIG.READ_API_KEY}&results=${THINGSPEAK_CONFIG.RESULTS}&status=true`;
  const resp = await fetch(url);
  if (!resp.ok) throw new Error(`HTTP ${resp.status}: ${resp.statusText}`);
  const json = await resp.json();
  if (!json?.feeds?.length) throw new Error("No feeds returned");
  return json.feeds.map((f) => {
    const packed = parseFloat(f.field6) || 0;
    const intPacked = Math.floor(packed);
    const ss = Math.floor(intPacked / 100000);
    const sc = Math.floor((intPacked % 100000) / 1000);
    const tempAnom = Math.floor((intPacked % 1000) / 100) === 1;
    const humAnom = Math.floor((intPacked % 100) / 10) === 1;
    const pressAnom = (intPacked % 10) === 1;

    let decodedShockState = "NORMAL";
    if (sc === 1) decodedShockState = "MINOR_BUMP";
    else if (sc === 2) decodedShockState = "MAJOR_IMPACT";
    else if (sc === 3) decodedShockState = "FREE_FALL";
    else if (sc === 4) decodedShockState = "TILT_ALERT";
    else if (sc === 5) decodedShockState = "VIBRATION";

    let decodedTransportState = "UNKNOWN";
    if (ss === 0) decodedTransportState = "STATIONARY";
    else if (ss === 1) decodedTransportState = "IN_TRANSIT";
    else if (ss === 2) decodedTransportState = "IMPACT_EVENT";
    else if (ss === 3) decodedTransportState = "LOADING";
    else if (ss === 4) decodedTransportState = "ENV_ALERT";

    // The readable status is the primary state source and also carries pressure.
    const statusParts = (f.status || "").split("|");
    const statusTransportState = statusParts[0]?.trim();
    const validTransportStates = ["STATIONARY", "IN_TRANSIT", "IMPACT_EVENT", "LOADING", "ENV_ALERT"];
    const transportState = validTransportStates.includes(statusTransportState)
      ? statusTransportState
      : decodedTransportState;
    const statusShockState = statusParts[1]?.trim();
    const validShockStates = ["NORMAL", "MINOR_BUMP", "MAJOR_IMPACT", "FREE_FALL", "TILT_ALERT", "VIBRATION"];
    const productState = validShockStates.includes(statusShockState)
      ? statusShockState
      : decodedShockState;
    const priority = statusParts.length >= 3 ? statusParts[2] : "NORMAL";
    const rfidValue = statusParts.length >= 4 ? statusParts[3] : "0";

    return {
      timestamp: f.created_at,
      time: fmtTime(f.created_at),
      temperature: safeFloat(f.field1),
      humidity: safeFloat(f.field2),
      pressure: safeFloat(f.field3, 2),
      shock: safeFloat(f.field7),
      productState,
      latitude: safeFloat(f.field4, 6),
      longitude: safeFloat(f.field5, 6),
      packedAiStatus: packed,
      transportState,
      tempAnomaly: tempAnom,
      humAnomaly: humAnom,
      pressAnomaly: pressAnom,
      anomalyScore: safeFloat(f.field8, 2),
      priority,
      rfid: (rfidValue === "0" || rfidValue === "") ? "NO TAG" : rfidValue
    };
  });
};

function useThingSpeak() {
  const [feeds, setFeeds] = useState([]);
  const [latest, setLatest] = useState(null);
  const [status, setStatus] = useState("idle");
  const [error, setError] = useState(null);
  const [lastFetched, setLastFetched] = useState(null);
  const timerRef = useRef(null);

  const fetchData = useCallback(async () => {
    setStatus("loading");
    try {
      const parsed = await fetchThingSpeakData();
      setFeeds(parsed);
      setLatest(parsed[parsed.length - 1] ?? null);
      setLastFetched(new Date());
      setStatus("ok");
      setError(null);
    } catch (err) {
      setStatus("error");
      setError(err.message || "Unknown error");
    }
  }, []);

  useEffect(() => {
    fetchData();
    timerRef.current = setInterval(fetchData, THINGSPEAK_CONFIG.POLL_INTERVAL_MS);
    return () => clearInterval(timerRef.current);
  }, [fetchData]);

  const anomalies = latest
    ? [
      (latest.productState === "MAJOR_IMPACT" || latest.productState === "FREE_FALL" ||
        latest.transportState === "IMPACT_EVENT" || (latest.shock != null && latest.shock >= 2.5)) &&
      `🚨 Impact Alert: ${latest.transportState === "IMPACT_EVENT" ? "IMPACT_EVENT" : latest.productState} detected! (Peak: ${latest.shock ?? "?"}G)`,
      latest.productState === "MINOR_BUMP" &&
      `⚠️ Minor Bump detected (Peak: ${latest.shock ?? "?"}G)`,
      latest.tempAnomaly && `🌡️ Temperature Anomaly Detected! Score: ${latest.anomalyScore}`,
      latest.humAnomaly && `💧 Humidity Anomaly Detected! Score: ${latest.anomalyScore}`
    ].filter(Boolean)
    : [];

  return { feeds, latest, status, error, lastFetched, anomalies, refetch: fetchData };
}

function AnomalyBanner({ anomalies, onDismiss }) {
  if (!anomalies.length) return null;
  return (
    <div style={{
      position: "sticky",
      top: 0,
      zIndex: 50,
      background: "linear-gradient(90deg, #ffebf0, #e7f7ff)",
      border: "1px solid #ffb4cf",
      padding: "0.85rem 1.25rem",
      borderRadius: 16,
      display: "flex",
      alignItems: "center",
      gap: "1rem",
      margin: "1rem 2rem",
      boxShadow: "0 16px 40px rgba(255,180,207,0.18)",
    }}>
      <span style={{ fontSize: 18, color: "#be123c" }}>⚠</span>
      <div style={{ flex: 1 }}>
        {anomalies.map((msg, i) => (
          <p key={i} style={{ margin: 0, fontFamily: "Inter, sans-serif", color: "#881337", fontWeight: 700 }}>
            {msg}
          </p>
        ))}
      </div>
      <button
        onClick={onDismiss}
        style={{
          background: "#ffffff",
          border: "1px solid #f9a8d4",
          borderRadius: 999,
          color: "#be123c",
          padding: "0.5rem 1rem",
          cursor: "pointer",
          fontWeight: 700,
        }}
      >
        Dismiss
      </button>
    </div>
  );
}

function MetricCard({ icon, label, value, unit, anomaly, sublabel }) {
  const [hovered, setHovered] = useState(false);
  const accent = anomaly ? "#FFB4CF" : hovered ? "#84CCFF" : "#7ABE80";
  return (
    <div
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      style={{
        background: hovered ? "linear-gradient(135deg, rgba(132,204,255,0.28), rgba(255,180,207,0.18))" : "#ffffff",
        border: `1px solid ${hovered ? "rgba(132,204,255,0.55)" : "rgba(15,23,42,0.08)"}`,
        borderRadius: 24,
        padding: "1.4rem 1.25rem",
        display: "flex",
        flexDirection: "column",
        gap: 10,
        boxShadow: hovered ? "0 20px 40px rgba(132,204,255,0.14)" : "0 12px 24px rgba(15,23,42,0.06)",
        transition: "all 0.25s ease",
      }}
    >
      <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between" }}>
        <span style={{ fontSize: 12, letterSpacing: "0.12em", color: "#64748b", textTransform: "uppercase" }}>{label}</span>
        <span style={{ fontSize: 20 }}>{icon}</span>
      </div>
      <div style={{ display: "flex", alignItems: "baseline", gap: 8 }}>
        <span style={{ fontSize: 34, fontWeight: 700, color: accent }}>{value ?? "--"}</span>
        {unit && <span style={{ fontSize: 14, color: "#64748b" }}>{unit}</span>}
      </div>
      <span style={{ fontSize: 12, color: "#94a3b8" }}>{sublabel}</span>
      {anomaly && (
        <span style={{ fontSize: 11, color: "#fb7185", fontWeight: 700 }}>ANOMALY</span>
      )}
    </div>
  );
}

function ChartPanel({ title, headerCenter, headerRight, children }) {
  return (
    <div style={{
      background: "#f8fdff",
      border: "1px solid rgba(132,204,255,0.28)",
      borderRadius: 24,
      padding: "1.25rem",
      boxShadow: "0 20px 40px rgba(132,204,255,0.15)",
      minWidth: 0,
      flex: 1,
    }}>
      <div style={{
        display: "grid",
        gridTemplateColumns: "minmax(0, 1fr) auto minmax(0, 1fr)",
        alignItems: "center",
        gap: "1rem",
      }}>
        <p style={{ margin: 0, fontSize: 13, fontWeight: 700, color: "#1e293b" }}>{title}</p>
        <div style={{ justifySelf: "center", textAlign: "center" }}>{headerCenter}</div>
        <div style={{ justifySelf: "end" }}>{headerRight}</div>
      </div>
      {children}
    </div>
  );
}

function EdgeAIAnalysisPanel({ latest }) {
  const Stat = ({ label, value, desc }) => (
    <div title={desc} style={{ cursor: "help" }}>
      <span style={{ display: "block", color: "#0f172a", fontWeight: 700, marginBottom: 2 }}>{label}</span>
      <span style={{ color: "#334155", fontWeight: 600, fontSize: "1.05rem" }}>{value}</span>
      <span style={{ display: "block", color: "#64748b", fontSize: "0.7rem", marginTop: 2, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{desc}</span>
    </div>
  );

  return (
    <div style={{
      background: "#fff6f6",
      border: "1px solid rgba(225,29,72,0.15)",
      borderRadius: 20,
      padding: "1.25rem",
      display: "grid",
      gridTemplateColumns: "repeat(auto-fit, minmax(150px, 1fr))",
      gap: "1rem",
      alignItems: "start",
      boxShadow: "0 20px 40px rgba(225,29,72,0.05)",
    }}>
      <Stat label="Shock Class" value={latest?.productState ?? "--"} desc="Edge classification" />
      <Stat label="Peak G-Force" value={latest?.shock != null ? `${latest.shock} G` : "--"} desc="Max impact" />
      <Stat label="Alert Priority" value={latest?.priority ?? "--"} desc="Transmission urgency" />
      <Stat
        label="Transport State"
        value={latest?.transportState ?? "UNKNOWN"}
        desc="Current journey state"
      />
    </div>
  );
}

function LocationMap({ latitude, longitude }) {
  const mapContainerRef = useRef(null);
  const [mapWidth, setMapWidth] = useState(0);

  const hasCoordinates =
    Number.isFinite(latitude) &&
    Number.isFinite(longitude) &&
    latitude >= -90 &&
    latitude <= 90 &&
    longitude >= -180 &&
    longitude <= 180;

  useEffect(() => {
    if (!hasCoordinates) return undefined;

    const container = mapContainerRef.current;
    if (!container) return undefined;

    const updateWidth = () => {
      const nextWidth = Math.round(container.getBoundingClientRect().width);
      setMapWidth((currentWidth) => currentWidth === nextWidth ? currentWidth : nextWidth);
    };

    updateWidth();
    const resizeObserver = new ResizeObserver(updateWidth);
    resizeObserver.observe(container);

    return () => resizeObserver.disconnect();
  }, [hasCoordinates]);

  if (!hasCoordinates) {
    return (
      <ChartPanel title="Live Asset Location">
        <div style={{
          minHeight: 340,
          display: "grid",
          placeItems: "center",
          borderRadius: 18,
          border: "1px dashed rgba(100,116,139,0.35)",
          background: "linear-gradient(135deg, rgba(132,204,255,0.08), rgba(122,190,128,0.08))",
          color: "#64748b",
          textAlign: "center",
          padding: "2rem",
        }}>
          <div>
            <div style={{ fontSize: 36, marginBottom: 10 }}>📍</div>
            <p style={{ margin: 0, fontWeight: 700, color: "#334155" }}>Waiting for GPS coordinates</p>
            <p style={{ margin: "0.4rem 0 0", fontSize: 13 }}>The map will appear when a valid location is received.</p>
          </div>
        </div>
      </ChartPanel>
    );
  }

  const mapSpan = 0.025;
  const bbox = [
    longitude - mapSpan,
    latitude - mapSpan,
    longitude + mapSpan,
    latitude + mapSpan,
  ].join(",");
  const latitudeText = fmtCoordinate(latitude);
  const longitudeText = fmtCoordinate(longitude);
  const embedUrl = `https://www.openstreetmap.org/export/embed.html?bbox=${encodeURIComponent(bbox)}&layer=mapnik&marker=${encodeURIComponent(`${latitudeText},${longitudeText}`)}`;
  const fullMapUrl = `https://www.openstreetmap.org/?mlat=${encodeURIComponent(latitudeText)}&mlon=${encodeURIComponent(longitudeText)}#map=15/${latitudeText}/${longitudeText}`;

  return (
    <ChartPanel
      title="Live Asset Location"
      headerCenter={
        <div>
          <span style={{ display: "block", color: "#64748b", fontSize: 10, letterSpacing: "0.1em", textTransform: "uppercase" }}>
            Current coordinates
          </span>
          <span style={{ color: "#0f172a", fontSize: 14, fontWeight: 700, fontVariantNumeric: "tabular-nums", whiteSpace: "nowrap" }}>
            {latitudeText}, {longitudeText}
          </span>
        </div>
      }
      headerRight={
        <a
          href={fullMapUrl}
          target="_blank"
          rel="noreferrer"
          style={{
            background: "#dbeafe",
            color: "#1d4ed8",
            borderRadius: 999,
            padding: "0.65rem 1rem",
            fontSize: 13,
            fontWeight: 700,
            textDecoration: "none",
            whiteSpace: "nowrap",
          }}
        >
          Open full map ↗
        </a>
      }
    >
      <div style={{
        marginTop: "1rem",
        borderRadius: 18,
        overflow: "hidden",
        border: "1px solid rgba(15,23,42,0.1)",
        background: "#e2e8f0",
        width: "100%",
        height: 420,
      }}>
        <div ref={mapContainerRef} style={{ width: "100%", height: "100%" }}>
          {mapWidth > 0 && (
            <iframe
              key={`${latitudeText}-${longitudeText}-${mapWidth}`}
              title={`Asset location at ${latitudeText}, ${longitudeText}`}
              src={embedUrl}
              width={mapWidth}
              height="420"
              loading="lazy"
              referrerPolicy="no-referrer-when-downgrade"
              style={{ width: "100%", height: "100%", border: 0, display: "block" }}
            />
          )}
        </div>
      </div>
      <div style={{
        display: "none",
        flexWrap: "wrap",
        alignItems: "center",
        justifyContent: "space-between",
        gap: "0.75rem",
        marginTop: "0.9rem",
      }}>
        <div>
          <span style={{ display: "block", color: "#64748b", fontSize: 11, letterSpacing: "0.1em", textTransform: "uppercase" }}>Current coordinates</span>
          <span style={{ color: "#0f172a", fontWeight: 700, fontVariantNumeric: "tabular-nums" }}>
            {latitudeText}, {longitudeText}
          </span>
        </div>
        <a
          href={fullMapUrl}
          target="_blank"
          rel="noreferrer"
          style={{
            background: "#dbeafe",
            color: "#1d4ed8",
            borderRadius: 999,
            padding: "0.65rem 1rem",
            fontSize: 13,
            fontWeight: 700,
            textDecoration: "none",
          }}
        >
          Open full map ↗
        </a>
      </div>
    </ChartPanel>
  );
}

function ChartTooltip({ active, payload, label, unit }) {
  if (!active || !payload?.length) return null;
  return (
    <div style={{
      background: "#ffffff",
      border: "1px solid rgba(15,23,42,0.12)",
      borderRadius: 16,
      padding: "0.75rem 1rem",
      fontSize: 12,
      color: "#0f172a",
      boxShadow: "0 20px 40px rgba(15,23,42,0.08)",
    }}>
      <p style={{ margin: 0, fontWeight: 700 }}>{label}</p>
      <p style={{ margin: 0 }}>{payload[0]?.value != null ? `${payload[0].value}${unit}` : "--"}</p>
    </div>
  );
}

function StatusOverlay({ status, error, onRetry }) {
  if (status === "ok" || status === "idle") return null;
  return (
    <div style={{
      position: "absolute",
      inset: 0,
      zIndex: 10,
      background: "rgba(255,255,255,0.92)",
      borderRadius: 24,
      display: "flex",
      flexDirection: "column",
      alignItems: "center",
      justifyContent: "center",
      gap: 12,
      padding: "1.5rem",
    }}>
      {status === "loading" && (
        <>
          <div style={{
            width: 44,
            height: 44,
            border: "4px solid rgba(56,189,248,0.25)",
            borderTopColor: "#38bdf8",
            borderRadius: "50%",
            animation: "spin 1s linear infinite",
          }} />
          <p style={{ margin: 0, fontWeight: 700, color: "#0f172a" }}>FETCHING FEEDS…</p>
        </>
      )}
      {status === "error" && (
        <>
          <p style={{ margin: 0, fontWeight: 700, color: "#be123c" }}>FETCH ERROR</p>
          <p style={{ margin: 0, maxWidth: 280, color: "#475569", textAlign: "center" }}>{error}</p>
          <button onClick={onRetry} style={{
            marginTop: 8,
            background: "#38bdf8",
            color: "#ffffff",
            border: "none",
            borderRadius: 999,
            padding: "0.6rem 1.2rem",
            cursor: "pointer",
            fontWeight: 700,
          }}>RETRY</button>
        </>
      )}
    </div>
  );
}

// ============================================================
// Background anomaly email alert hook
// Polls ThingSpeak when user is NOT logged in and sends
// an email via EmailJS when anomalies are detected.
// ============================================================
function useAnomalyEmailAlert(enabled) {
  const lastEmailSentRef = useRef(0);
  const timerRef = useRef(null);

  useEffect(() => {
    if (!enabled) {
      clearInterval(timerRef.current);
      return;
    }

    const checkAndAlert = async () => {
      try {
        const feeds = await fetchThingSpeakData();
        if (!feeds.length) return;
        const latest = feeds[feeds.length - 1];

        // Build anomaly messages using same logic as useThingSpeak
        const anomalyMessages = [
          (latest.productState === "MAJOR_IMPACT" || latest.productState === "FREE_FALL" ||
            latest.transportState === "IMPACT_EVENT" || (latest.shock != null && latest.shock >= 2.5)) &&
          `🚨 Impact Alert: ${latest.transportState === "IMPACT_EVENT" ? "IMPACT_EVENT" : latest.productState} detected! (Peak: ${latest.shock ?? "?"}G)`,
          latest.productState === "MINOR_BUMP" &&
          `⚠️ Minor Bump detected (Peak: ${latest.shock ?? "?"}G)`,
          latest.tempAnomaly && `🌡️ Temperature Anomaly Detected! Score: ${latest.anomalyScore}`,
          latest.humAnomaly && `💧 Humidity Anomaly Detected! Score: ${latest.anomalyScore}`,
        ].filter(Boolean);

        if (anomalyMessages.length === 0) return;

        // Enforce cooldown to prevent email spam
        const now = Date.now();
        if (now - lastEmailSentRef.current < EMAILJS_CONFIG.COOLDOWN_MS) return;

        // Send email
        const templateParams = {
          to_email: EMAILJS_CONFIG.TO_EMAIL,
          subject: `⚠️ Supply Chain Anomaly Alert`,
          message: anomalyMessages.join("\n"),
          temperature: latest.temperature != null ? `${latest.temperature}°C` : "--",
          humidity: latest.humidity != null ? `${latest.humidity}%` : "--",
          pressure: latest.pressure != null ? `${latest.pressure} hPa` : "--",
          shock: latest.shock != null ? `${latest.shock} G` : "--",
          transport_state: latest.transportState ?? "UNKNOWN",
          product_state: latest.productState ?? "UNKNOWN",
          timestamp: new Date(latest.timestamp).toLocaleString(),
        };

        await emailjs.send(
          EMAILJS_CONFIG.SERVICE_ID,
          EMAILJS_CONFIG.TEMPLATE_ID,
          templateParams,
          EMAILJS_CONFIG.PUBLIC_KEY
        );

        lastEmailSentRef.current = now;
        console.log("[AnomalyAlert] Email sent:", anomalyMessages);
      } catch (err) {
        console.error("[AnomalyAlert] Error:", err);
      }
    };

    // Check immediately, then on the polling interval
    checkAndAlert();
    timerRef.current = setInterval(checkAndAlert, THINGSPEAK_CONFIG.POLL_INTERVAL_MS);

    return () => clearInterval(timerRef.current);
  }, [enabled]);
}

function LoginPage({ onLogin }) {
  const [password, setPassword] = useState("");
  const [error, setError] = useState("");
  const inputRef = useRef(null);

  // Run anomaly email alerts while on the login page (not logged in)
  useAnomalyEmailAlert(true);

  useEffect(() => {
    inputRef.current?.focus();
  }, []);

  const handleSubmit = (event) => {
    event.preventDefault();

    if (password.length !== 6) {
      setError("Enter all 6 digits.");
      return;
    }

    if (password !== DASHBOARD_PASSWORD) {
      setError("Incorrect password. Please try again.");
      setPassword("");
      inputRef.current?.focus();
      return;
    }

    onLogin();
  };

  return (
    <div style={{
      minHeight: "100vh",
      display: "grid",
      placeItems: "center",
      padding: "1.5rem",
      background: "linear-gradient(145deg, #e8fbff 0%, #ecfff3 48%, #fff0f5 100%)",
      fontFamily: "'Plus Jakarta Sans', 'Inter', sans-serif",
      color: "#0f172a",
      overflow: "hidden",
      position: "relative",
    }}>
      <style>{`
        @import url('https://fonts.googleapis.com/css2?family=Plus+Jakarta+Sans:wght@400;500;600;700&display=swap');
        @keyframes loginFloat {
          0%, 100% { transform: translateY(0); }
          50% { transform: translateY(-12px); }
        }
        .login-input:focus {
          outline: none;
          border-color: #7dd3fc !important;
          box-shadow: 0 0 0 5px rgba(125, 211, 252, 0.2);
        }
        .login-button:hover {
          transform: translateY(-2px);
          box-shadow: 0 14px 28px rgba(99, 102, 241, 0.28) !important;
        }
      `}</style>

      <div style={{
        position: "absolute",
        width: 320,
        height: 320,
        borderRadius: "50%",
        background: "rgba(132, 204, 255, 0.22)",
        top: "-100px",
        right: "-70px",
        filter: "blur(2px)",
        animation: "loginFloat 7s ease-in-out infinite",
      }} />
      <div style={{
        position: "absolute",
        width: 260,
        height: 260,
        borderRadius: "50%",
        background: "rgba(255, 180, 207, 0.2)",
        bottom: "-90px",
        left: "-50px",
        animation: "loginFloat 8s ease-in-out infinite reverse",
      }} />

      <main style={{
        width: "min(100%, 460px)",
        padding: "2.5rem",
        borderRadius: 32,
        background: "rgba(255, 255, 255, 0.74)",
        border: "1px solid rgba(255, 255, 255, 0.95)",
        boxShadow: "0 28px 70px rgba(15, 23, 42, 0.12)",
        backdropFilter: "blur(20px)",
        WebkitBackdropFilter: "blur(20px)",
        position: "relative",
        zIndex: 1,
      }}>
        <div style={{
          width: 68,
          height: 68,
          display: "grid",
          placeItems: "center",
          marginBottom: "1.5rem",
          borderRadius: 22,
          background: "linear-gradient(135deg, #bfdbfe, #bbf7d0)",
          fontSize: 32,
          boxShadow: "0 12px 24px rgba(56, 189, 248, 0.18)",
        }}>
          🚚
        </div>

        <p style={{ margin: 0, color: "#64748b", fontSize: 12, fontWeight: 700, letterSpacing: "0.18em", textTransform: "uppercase" }}>
          Supply Chain IoT
        </p>
        <h1 style={{ margin: "0.65rem 0 0", fontSize: 34, letterSpacing: "-0.035em" }}>
          Welcome back
        </h1>
        <p style={{ margin: "0.75rem 0 2rem", color: "#64748b", lineHeight: 1.65 }}>
          Enter your password to access the asset monitoring dashboard.
        </p>

        <form onSubmit={handleSubmit}>
          <label htmlFor="dashboard-password" style={{ display: "block", marginBottom: "0.6rem", color: "#334155", fontSize: 13, fontWeight: 700 }}>
            Enter password
          </label>
          <input
            ref={inputRef}
            id="dashboard-password"
            className="login-input"
            type="password"
            inputMode="numeric"
            pattern="[0-9]{6}"
            autoComplete="off"
            maxLength={6}
            value={password}
            onChange={(event) => {
              setPassword(event.target.value.replace(/\D/g, "").slice(0, 6));
              setError("");
            }}
            aria-invalid={Boolean(error)}
            style={{
              width: "100%",
              boxSizing: "border-box",
              border: `1px solid ${error ? "#fb7185" : "rgba(100,116,139,0.25)"}`,
              borderRadius: 18,
              background: "rgba(255,255,255,0.9)",
              padding: "1rem 1.1rem",
              color: "#0f172a",
              fontFamily: "inherit",
              fontSize: 24,
              fontWeight: 700,
              letterSpacing: "0.42em",
              textAlign: "center",
              transition: "all 0.2s ease",
            }}
          />
          <div style={{ minHeight: 31, paddingTop: "0.55rem" }}>
            {error && <span style={{ color: "#e11d48", fontSize: 12, fontWeight: 600 }}>{error}</span>}
          </div>
          <button
            className="login-button"
            type="submit"
            disabled={password.length !== 6}
            style={{
              width: "100%",
              border: 0,
              borderRadius: 18,
              padding: "1rem",
              background: password.length === 6 ? "linear-gradient(135deg, #818cf8, #38bdf8)" : "#cbd5e1",
              color: "#ffffff",
              cursor: password.length === 6 ? "pointer" : "not-allowed",
              fontFamily: "inherit",
              fontSize: 15,
              fontWeight: 700,
              boxShadow: password.length === 6 ? "0 10px 22px rgba(99, 102, 241, 0.22)" : "none",
              transition: "all 0.2s ease",
            }}
          >
            Open Dashboard
          </button>
        </form>
      </main>
    </div>
  );
}

function DashboardContent() {
  const { feeds, latest, status, error, lastFetched, anomalies, refetch } = useThingSpeak();
  const [dismissed, setDismissed] = useState(false);
  const prevAnomalyKey = useRef("");

  const anomalyKey = anomalies.join("|");
  useEffect(() => {
    if (anomalyKey !== prevAnomalyKey.current) {
      setDismissed(false);
      prevAnomalyKey.current = anomalyKey;
    }
  }, [anomalyKey]);

  const shockAnomaly = latest?.productState === "MAJOR_IMPACT" || latest?.productState === "FREE_FALL" ||
    latest?.transportState === "IMPACT_EVENT" || (latest?.shock != null && latest.shock >= 2.5);

  const chartData = feeds.map((f) => ({
    time: f.time,
    temperature: f.temperature,
    shock: f.shock,
    humidity: f.humidity,
    pressure: f.pressure,
  }));

  return (
    <div style={{
      minHeight: "100vh",
      background: "linear-gradient(180deg, #f3fdff 0%, #e7fff1 45%, #fff0f4 100%)",
      color: "#0f172a",
      fontFamily: "'Plus Jakarta Sans', 'Inter', sans-serif",
      padding: "1rem 1rem 2rem",
    }}>
      <style>{`
        @import url('https://fonts.googleapis.com/css2?family=Plus+Jakarta+Sans:wght@400;500;600;700&display=swap');
        @keyframes spin { to { transform: rotate(360deg); } }
        .glass-header {
          background: rgba(255, 255, 255, 0.65);
          backdrop-filter: blur(16px);
          -webkit-backdrop-filter: blur(16px);
          border: 1px solid rgba(255, 255, 255, 1);
          border-radius: 28px;
          padding: 2rem 2.5rem;
          box-shadow: 0 12px 36px rgba(15, 23, 42, 0.03), inset 0 1px 0 rgba(255,255,255,1);
        }
        .refresh-btn {
          background: #c7d2fe;
          color: #312e81;
          border: 2px solid #ffffff;
          border-radius: 999px;
          padding: 0.85rem 1.75rem;
          font-weight: 700;
          font-family: inherit;
          font-size: 1rem;
          cursor: pointer;
          box-shadow: 0 8px 20px rgba(199, 210, 254, 0.6);
          transition: all 0.25s cubic-bezier(0.4, 0, 0.2, 1);
          display: inline-flex;
          align-items: center;
          gap: 0.5rem;
        }
        .refresh-btn:hover {
          transform: translateY(-2px);
          box-shadow: 0 12px 25px rgba(199, 210, 254, 0.8);
          background: #a5b4fc;
        }
        .refresh-btn:active {
          transform: translateY(1px);
        }
      `}</style>

      {!dismissed && anomalies.length > 0 && <AnomalyBanner anomalies={anomalies} onDismiss={() => setDismissed(true)} />}

      <header className="glass-header" style={{
        maxWidth: 1360,
        margin: "0 auto 1.5rem",
        display: "flex",
        flexWrap: "wrap",
        alignItems: "center",
        justifyContent: "space-between",
        gap: "1.5rem",
      }}>
        <div>
          <p style={{ margin: 0, fontSize: 13, letterSpacing: "0.25em", color: "#64748b", textTransform: "uppercase", fontWeight: 700 }}>Supply Chain IoT</p>
          <h1 style={{ margin: "0.5rem 0 0", fontSize: 36, lineHeight: 1.1, color: "#0f172a", letterSpacing: "-0.03em" }}>Asset Monitoring Dashboard</h1>
          <p style={{ margin: "0.85rem 0 0", color: "#475569", maxWidth: 650, fontSize: "1.05rem", lineHeight: 1.6 }}>Direct ThingSpeak integration with fresh telemetry, friendly visuals, and clean metrics for your supply chain assets.</p>
        </div>
        <div style={{ display: "flex", alignItems: "center", gap: "1.5rem" }}>
          <div style={{ textAlign: "right" }}>
            <span style={{ display: "block", color: status === "ok" ? "#16a34a" : status === "loading" ? "#f59e0b" : "#dc2626", fontWeight: 700, fontSize: 14 }}>
              {status === "loading" ? "Fetching..." : status === "ok" ? "Connected" : "Error"}
            </span>
            <span style={{ display: "block", color: "#64748b", fontSize: 12 }}>
              Last sync: {lastFetched ? lastFetched.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" }) : "--"}
            </span>
          </div>
          <button onClick={refetch} className="refresh-btn">
            <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round">
              <path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8" />
              <path d="M3 3v5h5" />
            </svg>
            Refresh Now
          </button>
        </div>
      </header>

      <main style={{
        maxWidth: 1360,
        margin: "0 auto",
        display: "grid",
        gap: "1.25rem",
      }}>
        <EdgeAIAnalysisPanel latest={latest} />

        <section style={{
          display: "grid",
          gridTemplateColumns: "repeat(auto-fit, minmax(220px, 1fr))",
          gap: "1rem",
        }}>
          <MetricCard
            icon="🌡"
            label="Temperature"
            value={latest?.temperature}
            unit="°C"
            anomaly={false}
            sublabel="Edge AI managed"
          />
          <MetricCard
            icon="💧"
            label="Humidity"
            value={latest?.humidity}
            unit="%"
            anomaly={latest?.humAnomaly}
            sublabel="Relative humidity"
          />
          <MetricCard
            icon="P"
            label="Pressure"
            value={latest?.pressure}
            unit="hPa"
            anomaly={latest?.pressAnomaly}
            sublabel="Atmospheric pressure"
          />
          <div style={{ background: "#ffffff", border: "1px solid rgba(15,23,42,0.08)", borderRadius: 24, padding: "1.4rem 1.25rem", display: "flex", flexDirection: "column", gap: 10, boxShadow: "0 12px 24px rgba(15,23,42,0.06)" }}>
            <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between" }}>
              <span style={{ fontSize: 12, letterSpacing: "0.12em", color: "#64748b", textTransform: "uppercase" }}>RFID Panel</span>
              <span style={{ fontSize: 20 }}>📡</span>
            </div>
            <div style={{ display: "flex", alignItems: "baseline", gap: 8 }}>
              <span style={{ fontSize: 26, fontWeight: 700, color: "#334155" }}>{latest?.rfid ?? "NO TAG"}</span>
            </div>
            <span style={{ fontSize: 12, color: "#94a3b8" }}>Only accepts RFID tag scans</span>
          </div>
        </section>

        <section>
          <LocationMap latitude={latest?.latitude} longitude={latest?.longitude} />
        </section>

        <section style={{ display: "grid", gap: "1rem" }}>
          <div style={{ display: "grid", gridTemplateColumns: "1.5fr 1fr", gap: "1rem" }}>
            <ChartPanel title="Temperature Trend (°C)">
              <div style={{ position: "relative", minHeight: 240 }}>
                <StatusOverlay status={status} error={error} onRetry={refetch} />
                <ResponsiveContainer width="100%" height={240}>
                  <AreaChart data={chartData} margin={{ top: 10, right: 20, left: 0, bottom: 0 }}>
                    <defs>
                      <linearGradient id="tempGrad" x1="0" y1="0" x2="0" y2="1">
                        <stop offset="5%" stopColor="#FFB4CF" stopOpacity={0.45} />
                        <stop offset="95%" stopColor="#FFB4CF" stopOpacity={0.08} />
                      </linearGradient>
                    </defs>
                    <CartesianGrid strokeDasharray="3 3" stroke="#e2e8f0" />
                    <XAxis dataKey="time" tick={{ fill: "#64748b", fontSize: 12 }} tickLine={false} axisLine={false} />
                    <YAxis tick={{ fill: "#64748b", fontSize: 12 }} tickLine={false} axisLine={false} />
                    <Tooltip content={<ChartTooltip unit="°C" />} />

                    <Area type="monotone" dataKey="temperature" stroke="#fb7185" strokeWidth={2.5} fill="url(#tempGrad)" dot={false} />
                  </AreaChart>
                </ResponsiveContainer>
              </div>
            </ChartPanel>
            <ChartPanel title="Shock Trend (m/s²)">
              <div style={{ position: "relative", minHeight: 240 }}>
                <StatusOverlay status={status} error={error} onRetry={refetch} />
                <ResponsiveContainer width="100%" height={240}>
                  <AreaChart data={chartData} margin={{ top: 10, right: 20, left: 0, bottom: 0 }}>
                    <defs>
                      <linearGradient id="shockGrad" x1="0" y1="0" x2="0" y2="1">
                        <stop offset="5%" stopColor="#84CCFF" stopOpacity={0.4} />
                        <stop offset="95%" stopColor="#84CCFF" stopOpacity={0.08} />
                      </linearGradient>
                    </defs>
                    <CartesianGrid strokeDasharray="3 3" stroke="#e2e8f0" />
                    <XAxis dataKey="time" tick={{ fill: "#64748b", fontSize: 12 }} tickLine={false} axisLine={false} />
                    <YAxis tick={{ fill: "#64748b", fontSize: 12 }} tickLine={false} axisLine={false} />
                    <Tooltip content={<ChartTooltip unit="m/s²" />} />

                    <Area type="monotone" dataKey="shock" stroke="#38bdf8" strokeWidth={2.5} fill="url(#shockGrad)" dot={false} />
                  </AreaChart>
                </ResponsiveContainer>
              </div>
            </ChartPanel>
          </div>

          <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "1rem" }}>
            <ChartPanel title="Humidity Trend (%)">
              <div style={{ position: "relative", minHeight: 220 }}>
                <StatusOverlay status={status} error={error} onRetry={refetch} />
                <ResponsiveContainer width="100%" height={220}>
                  <AreaChart data={chartData} margin={{ top: 10, right: 20, left: 0, bottom: 0 }}>
                    <defs>
                      <linearGradient id="humGrad" x1="0" y1="0" x2="0" y2="1">
                        <stop offset="5%" stopColor="#7ABE80" stopOpacity={0.35} />
                        <stop offset="95%" stopColor="#7ABE80" stopOpacity={0.08} />
                      </linearGradient>
                    </defs>
                    <CartesianGrid strokeDasharray="3 3" stroke="#e2e8f0" />
                    <XAxis dataKey="time" tick={{ fill: "#64748b", fontSize: 12 }} tickLine={false} axisLine={false} />
                    <YAxis tick={{ fill: "#64748b", fontSize: 12 }} tickLine={false} axisLine={false} />
                    <Tooltip content={<ChartTooltip unit="%" />} />
                    <Area type="monotone" dataKey="humidity" stroke="#22c55e" strokeWidth={2.5} fill="url(#humGrad)" dot={false} />
                  </AreaChart>
                </ResponsiveContainer>
              </div>
            </ChartPanel>
            <ChartPanel title="Recent Feed Log">
              <div style={{ minHeight: 220, overflowX: "auto" }}>
                <table style={{ width: "100%", borderCollapse: "collapse", fontSize: 12, color: "#334155" }}>
                  <thead>
                    <tr>
                      {['Time', 'State', 'Shock', 'Temp', 'Hum', 'PeakG'].map((heading) => (
                        <th key={heading} style={{ textAlign: 'left', padding: '0.85rem 0.5rem', color: '#475569', fontWeight: 700 }}>{heading}</th>
                      ))}
                    </tr>
                  </thead>
                  <tbody>
                    {feeds.slice(-8).reverse().map((feed, index) => (
                      <tr key={index} style={{ borderTop: '1px solid rgba(15,23,42,0.08)' }}>
                        <td style={{ padding: '0.9rem 0.5rem' }}>{feed.time}</td>
                        <td style={{ padding: '0.9rem 0.5rem' }}>{feed.transportState}</td>
                        <td style={{ padding: '0.9rem 0.5rem' }}>{feed.productState}</td>
                        <td style={{ padding: '0.9rem 0.5rem' }}>{feed.temperature != null ? `${feed.temperature}°C` : '--'}</td>
                        <td style={{ padding: '0.9rem 0.5rem' }}>{feed.humidity != null ? `${feed.humidity}%` : '--'}</td>
                        <td style={{ padding: '0.9rem 0.5rem' }}>{feed.shock != null ? `${feed.shock}G` : '--'}</td>
                      </tr>
                    ))}
                    {feeds.length === 0 && (
                      <tr>
                        <td colSpan={5} style={{ padding: '1rem', textAlign: 'center', color: '#64748b' }}>No data available</td>
                      </tr>
                    )}
                  </tbody>
                </table>
              </div>
            </ChartPanel>
          </div>
        </section>
      </main>
    </div>
  );
}

export default function IotDashboard() {
  const [authenticated, setAuthenticated] = useState(false);

  return authenticated
    ? <DashboardContent />
    : <LoginPage onLogin={() => setAuthenticated(true)} />;
}
