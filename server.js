const express = require("express");
const cors = require("cors");
const db = require("./firebase");

const app = express();
const PORT = 5000;

app.use(cors());
app.use(express.json());

// Verify Firebase SDK connection
db.listCollections()
  .then(() => {
    console.log("✅ Firebase Admin SDK initialized successfully!");
  })
  .catch((err) => {
    console.error("❌ Firebase initialization failed:", err.message);
  });


// ===============================
// ESP32 TELEMETRY ROUTE
// ===============================
app.post("/telemetry", async (req, res) => {
  try {
    const data = req.body;

    const docRef = await db.collection("telemetry").add({
      ...data,
      timestamp: new Date()
    });

    console.log("Telemetry saved:", docRef.id);

    res.status(200).json({
      success: true,
      id: docRef.id
    });

  } catch (error) {
    console.error("Telemetry save error:", error);

    res.status(500).json({
      success: false,
      error: error.message
    });
  }
});


// ===============================
// START SERVER
// ===============================
app.listen(PORT, "0.0.0.0", () => {
  console.log(`Server running on http://0.0.0.0:${PORT}`);
});