const db = require("./firebase");
const XLSX = require("xlsx");

async function exportTelemetry() {
  try {
    console.log("Reading telemetry data from Firestore...");

    const snapshot = await db
      .collection("telemetry")
      .orderBy("timestamp", "asc")
      .get();

    if (snapshot.empty) {
      console.log("No telemetry data found.");
      return;
    }

    const data = snapshot.docs.map((doc) => {
      const d = doc.data();

      return {
        Timestamp: d.timestamp
          ? d.timestamp.toDate().toLocaleString()
          : "",
        Temperature: d.temperature ?? "",
        Current: d.current ?? "",
        Vibration: d.vibration ?? "",
        MotorStatus: d.motorStatus ?? "",
        BeltHealth: d.beltHealth ?? "",
        HealthState: d.healthState ?? "",
        TemperatureValid: d.temperatureValid ?? "",
        EmergencyStop: d.emergencyStop ?? "",
        TripReason: d.tripReason ?? "",
        SystemMessage: d.systemMessage ?? ""
      };
    });

    const worksheet = XLSX.utils.json_to_sheet(data);
    const workbook = XLSX.utils.book_new();

    XLSX.utils.book_append_sheet(
      workbook,
      worksheet,
      "Telemetry"
    );

    XLSX.writeFile(workbook, "telemetry.xlsx");

    console.log("✅ Excel file created successfully!");
    console.log(`Total records exported: ${data.length}`);
    console.log("File: telemetry.xlsx");

  } catch (error) {
    console.error("❌ Export failed:", error);
  }
}

exportTelemetry();
