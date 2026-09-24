#include <WiFi.h>
#include <WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <HTTPClient.h>

// ============================================================
// WIFI
// ============================================================

const char* ssid = "Airtel_HOMIES......";
const char* password = "6_Perutaiyum_Kelu";

const char* backendURL =
    "http://192.168.1.5:5000/telemetry";

// ============================================================
// PIN CONNECTIONS
// ============================================================

#define ACS712_PIN 34
#define DS18B20_PIN 4
#define RELAY_PIN 18

// ============================================================
// RELAY LOGIC
// ============================================================

#define RELAY_ON LOW
#define RELAY_OFF HIGH

// ============================================================
// ACS712
// ============================================================

const float ACS_SENSITIVITY = 0.185;

float ACS_ZERO_VOLTAGE = 2.50;

const float ADC_REFERENCE = 3.3;
const float ADC_MAX = 4095.0;

// ============================================================
// SAFETY LIMITS
// ============================================================

const float CURRENT_WARNING = 2.0;
const float CURRENT_LIMIT = 2.5;

const float TEMP_WARNING = 45.0;
const float TEMP_LIMIT = 60.0;

// ============================================================
// OBJECTS
// ============================================================

WebServer server(80);

OneWire oneWire(DS18B20_PIN);

DallasTemperature temperatureSensor(&oneWire);

// ============================================================
// VARIABLES
// ============================================================

float currentValue = 0.0;

float temperatureValue = 0.0;

bool temperatureValid = false;

bool ds18b20Detected = false;

int beltHealth = 100;

bool motorRunning = false;

bool emergencyStop = false;

String tripReason = "None";

unsigned long lastSensorRead = 0;

// ============================================================
// MOTOR ON
// ============================================================

void motorON()
{
    digitalWrite(RELAY_PIN, RELAY_ON);

    motorRunning = true;

    Serial.println("RELAY: ON");
    Serial.println("MOTOR: RUNNING");
}

// ============================================================
// MOTOR OFF
// ============================================================

void motorOFF()
{
    digitalWrite(RELAY_PIN, RELAY_OFF);

    motorRunning = false;

    Serial.println("RELAY: OFF");
    Serial.println("MOTOR: OFF");
}

// ============================================================
// ACS712 ZERO CALIBRATION
// ============================================================

void calibrateACS712()
{
    Serial.println();
    Serial.println("======================================");
    Serial.println("       ACS712 CALIBRATION");
    Serial.println("======================================");

    Serial.println("Motor must be OFF.");
    Serial.println("Calibrating zero-current voltage...");

    delay(1000);

    const int samples = 500;

    long totalADC = 0;

    for (int i = 0; i < samples; i++)
    {
        totalADC += analogRead(ACS712_PIN);
        delay(2);
    }

    float averageADC =
        totalADC / (float)samples;

    ACS_ZERO_VOLTAGE =
        (averageADC / ADC_MAX) *
        ADC_REFERENCE;

    Serial.print("ACS712 Zero Voltage: ");

    Serial.print(
        ACS_ZERO_VOLTAGE,
        3
    );

    Serial.println(" V");

    Serial.println("Calibration complete.");

    Serial.println("======================================");
}

// ============================================================
// READ CURRENT
// ============================================================

float readCurrent()
{
    const int samples = 100;

    long totalADC = 0;

    for (int i = 0; i < samples; i++)
    {
        totalADC += analogRead(ACS712_PIN);

        delayMicroseconds(200);
    }

    float averageADC =
        totalADC / (float)samples;

    float voltage =
        (averageADC / ADC_MAX) *
        ADC_REFERENCE;

    float current =
        (voltage - ACS_ZERO_VOLTAGE)
        / ACS_SENSITIVITY;

    current = abs(current);

    if (current < 0.08)
    {
        current = 0.0;
    }

    return current;
}

// ============================================================
// READ TEMPERATURE
// ============================================================

void readTemperature()
{
    temperatureSensor.requestTemperatures();

    float temp =
        temperatureSensor.getTempCByIndex(0);

    if (
        temp == DEVICE_DISCONNECTED_C ||
        temp <= -100 ||
        temp > 125
    )
    {
        temperatureValid = false;

        temperatureValue = 0.0;
    }
    else
    {
        temperatureValid = true;

        temperatureValue = temp;
    }
}

// ============================================================
// HEALTH STATE
// ============================================================

String getHealthState()
{
    if (beltHealth >= 80)
        return "NORMAL";

    if (beltHealth >= 50)
        return "WARNING";

    return "CRITICAL";
}

// ============================================================
// SYSTEM MESSAGE
// ============================================================

String getSystemMessage()
{
    if (emergencyStop)
        return "EMERGENCY STOP ACTIVE";

    if (!ds18b20Detected)
        return "Temperature Sensor Not Detected";

    if (!temperatureValid)
        return "Temperature Reading Error";

    if (currentValue >= CURRENT_WARNING)
        return "High Motor Current";

    if (temperatureValue >= TEMP_WARNING)
        return "High Temperature";

    if (!motorRunning)
        return "System Ready - Motor OFF";

    return "System Operating Normally";
}

// ============================================================
// SEND TELEMETRY TO NODE.JS
// ============================================================

void sendTelemetry()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("Telemetry: WiFi disconnected");
        return;
    }

    HTTPClient http;

    http.begin(backendURL);

    http.addHeader(
        "Content-Type",
        "application/json"
    );

    String json = "{";

    json += "\"temperature\":";
    json += String(
        temperatureValue,
        2
    );

    json += ",\"vibration\":0";

    json += ",\"current\":";
    json += String(
        currentValue,
        2
    );

    // ========================================================
    // FIXED MOTOR STATUS
    // ========================================================

    json += ",\"motorStatus\":\"";

    if (motorRunning)
    {
        json += "RUNNING";
    }
    else
    {
        json += "OFF";
    }

    json += "\"";

    json += ",\"beltHealth\":";
    json += String(
        beltHealth
    );

    json += ",\"healthState\":\"";
    json += getHealthState();
    json += "\"";

    json += ",\"temperatureValid\":";
    json += temperatureValid
            ? "true"
            : "false";

    json += ",\"emergencyStop\":";
    json += emergencyStop
            ? "true"
            : "false";

    json += ",\"tripReason\":\"";
    json += tripReason;
    json += "\"";

    json += ",\"systemMessage\":\"";
    json += getSystemMessage();
    json += "\"";

    json += "}";

    Serial.println();
    Serial.println("Sending telemetry...");
    Serial.println(json);

    int httpResponseCode =
        http.POST(json);

    Serial.print("Backend response: ");
    Serial.println(httpResponseCode);

    if (httpResponseCode > 0)
    {
        String response =
            http.getString();

        Serial.println("Backend message:");
        Serial.println(response);
    }
    else
    {
        Serial.print("HTTP Error: ");
        Serial.println(
            http.errorToString(
                httpResponseCode
            )
        );
    }

    http.end();
}

// ============================================================
// CALCULATE BELT HEALTH
// ============================================================

void calculateBeltHealth()
{
    int health = 100;

    // CURRENT

    if (
        currentValue > CURRENT_WARNING &&
        currentValue < CURRENT_LIMIT
    )
    {
        float penalty =
            (
                (currentValue - CURRENT_WARNING) /
                (CURRENT_LIMIT - CURRENT_WARNING)
            ) * 40.0;

        health -= (int)penalty;
    }
    else if (
        currentValue >= CURRENT_LIMIT
    )
    {
        health -= 50;
    }

    // TEMPERATURE

    if (temperatureValid)
    {
        if (
            temperatureValue > TEMP_WARNING &&
            temperatureValue < TEMP_LIMIT
        )
        {
            float penalty =
                (
                    (temperatureValue - TEMP_WARNING) /
                    (TEMP_LIMIT - TEMP_WARNING)
                ) * 40.0;

            health -= (int)penalty;
        }
        else if (
            temperatureValue >= TEMP_LIMIT
        )
        {
            health -= 50;
        }
    }

    if (health < 0)
        health = 0;

    if (health > 100)
        health = 100;

    beltHealth = health;
}

// ============================================================
// SAFETY CHECK
// ============================================================

void safetyCheck()
{
    // OVERCURRENT

    if (currentValue >= CURRENT_LIMIT)
    {
        emergencyStop = true;

        tripReason =
            "Overcurrent / Mechanical Jam";

        motorOFF();

        Serial.println();
        Serial.println("!!! SAFETY TRIP !!!");
        Serial.println("Overcurrent detected.");
        Serial.println("Motor stopped.");

        return;
    }

    // OVERTEMPERATURE

    if (
        temperatureValid &&
        temperatureValue >= TEMP_LIMIT
    )
    {
        emergencyStop = true;

        tripReason =
            "High Motor Temperature";

        motorOFF();

        Serial.println();
        Serial.println("!!! SAFETY TRIP !!!");
        Serial.println("High temperature detected.");
        Serial.println("Motor stopped.");

        return;
    }
}

// ============================================================
// SERIAL STATUS
// ============================================================

void printStatus()
{
    Serial.println();
    Serial.println("======================================");
    Serial.println("      CONVEYOR MONITORING SYSTEM");
    Serial.println("======================================");

    Serial.print("Motor Status : ");

    // FIXED: OFF instead of START

    if (motorRunning)
        Serial.println("RUNNING");
    else
        Serial.println("OFF");

    Serial.print("Current      : ");

    Serial.print(
        currentValue,
        2
    );

    Serial.println(" A");

    Serial.print("Temperature  : ");

    if (temperatureValid)
    {
        Serial.print(
            temperatureValue,
            2
        );

        Serial.println(" C");
    }
    else
    {
        Serial.println("SENSOR ERROR");
    }

    Serial.print("Belt Health  : ");

    Serial.print(
        beltHealth
    );

    Serial.println(" %");

    Serial.print("Health State : ");

    Serial.println(
        getHealthState()
    );

    Serial.print("System       : ");

    Serial.println(
        getSystemMessage()
    );

    Serial.print("Trip Reason  : ");

    Serial.println(
        tripReason
    );

    Serial.println("--------------------------------------");

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.print("Dashboard IP : http://");

        Serial.println(
            WiFi.localIP()
        );
    }

    Serial.println("======================================");
}

// ============================================================
// DASHBOARD HTML
// ============================================================

const char dashboardHTML[] PROGMEM = R"rawliteral(

<!DOCTYPE html>

<html>

<head>

<meta name="viewport"
content="width=device-width, initial-scale=1.0">

<title>
Conveyor Belt Monitoring
</title>

<style>

*{
    box-sizing:border-box;
    margin:0;
    padding:0;
}

body{
    font-family:Arial,Helvetica,sans-serif;
    background:linear-gradient(135deg,#0f172a,#111827);
    color:#f8fafc;
    min-height:100vh;
}

.header{
    padding:22px;
    background:rgba(15,23,42,.95);
    border-bottom:1px solid #26344d;
    position:sticky;
    top:0;
    z-index:10;
}

.header-content{
    max-width:1200px;
    margin:auto;
    display:flex;
    justify-content:space-between;
    align-items:center;
    gap:15px;
}

.title h1{
    font-size:25px;
}

.title p{
    margin-top:6px;
    color:#94a3b8;
}

.connection{
    display:flex;
    align-items:center;
    gap:8px;
    padding:9px 13px;
    border-radius:30px;
    background:#10251d;
    color:#4ade80;
    font-size:12px;
    font-weight:bold;
}

.dot{
    width:9px;
    height:9px;
    border-radius:50%;
    background:#4ade80;
    box-shadow:0 0 12px #4ade80;
}

.container{
    max-width:1200px;
    margin:auto;
    padding:22px;
}

.alert{
    padding:18px;
    border-radius:16px;
    margin-bottom:20px;
    border:1px solid #26344d;
    background:rgba(30,41,59,.75);
}

.alert.danger{
    border-color:#ef4444;
    background:rgba(127,29,29,.25);
}

.alert.warning{
    border-color:#f59e0b;
    background:rgba(120,53,15,.25);
}

.alert-title{
    font-size:12px;
    color:#94a3b8;
    margin-bottom:7px;
}

.alert-value{
    font-size:22px;
    font-weight:bold;
}

.grid{
    display:grid;
    grid-template-columns:repeat(auto-fit,minmax(220px,1fr));
    gap:17px;
}

.card{
    padding:20px;
    border-radius:18px;
    background:rgba(30,41,59,.72);
    border:1px solid #26344d;
    backdrop-filter:blur(10px);
    box-shadow:0 12px 35px rgba(0,0,0,.18);
}

.card-title{
    color:#94a3b8;
    font-size:12px;
    font-weight:bold;
    letter-spacing:.08em;
    margin-bottom:15px;
}

.metric{
    display:flex;
    align-items:end;
    gap:8px;
}

.metric-value{
    font-size:34px;
    font-weight:800;
}

.metric-unit{
    color:#94a3b8;
    margin-bottom:6px;
}

.status{
    display:inline-flex;
    align-items:center;
    gap:7px;
    margin-top:12px;
    padding:7px 11px;
    border-radius:20px;
    font-size:12px;
    font-weight:bold;
    background:#172033;
}

.status-dot{
    width:8px;
    height:8px;
    border-radius:50%;
    background:#64748b;
}

.motor-control{
    margin-top:18px;
}

.switch-row{
    display:flex;
    justify-content:space-between;
    align-items:center;
    gap:20px;
}

.switch-title{
    font-size:18px;
    font-weight:700;
}

.switch-description{
    margin-top:6px;
    color:#94a3b8;
    font-size:13px;
}

.switch{
    position:relative;
    display:inline-block;
    width:64px;
    height:34px;
    flex-shrink:0;
}

.switch input{
    opacity:0;
    width:0;
    height:0;
}

.slider{
    position:absolute;
    cursor:pointer;
    top:0;
    left:0;
    right:0;
    bottom:0;
    background:#334155;
    border-radius:40px;
    transition:.3s;
}

.slider:before{
    content:"";
    position:absolute;
    height:26px;
    width:26px;
    left:4px;
    top:4px;
    background:white;
    border-radius:50%;
    transition:.3s;
}

.switch input:checked + .slider{
    background:#16a34a;
}

.switch input:checked + .slider:before{
    transform:translateX(30px);
}

.health-card{
    margin-top:18px;
}

.health-header{
    display:flex;
    justify-content:space-between;
    align-items:center;
}

.health-score{
    font-size:28px;
    font-weight:800;
}

.progress{
    height:18px;
    background:#1e293b;
    border-radius:20px;
    margin-top:18px;
    overflow:hidden;
}

.progress-fill{
    height:100%;
    width:0%;
    transition:width .5s;
}

.info{
    margin-top:18px;
}

.info-row{
    display:flex;
    justify-content:space-between;
    gap:20px;
    padding:13px 0;
    border-bottom:1px solid #26344d;
}

.info-row:last-child{
    border-bottom:none;
}

.info-label{
    color:#94a3b8;
}

.info-value{
    font-weight:bold;
    text-align:right;
}

.controls{
    margin-top:18px;
    display:grid;
    grid-template-columns:repeat(auto-fit,minmax(180px,1fr));
    gap:14px;
}

button{
    border:none;
    padding:16px;
    border-radius:13px;
    color:white;
    font-size:14px;
    font-weight:800;
    cursor:pointer;
}

.stop{
    background:linear-gradient(135deg,#dc2626,#991b1b);
}

.reset{
    background:linear-gradient(135deg,#16a34a,#047857);
}

.temp-normal{
    color:#38bdf8;
}

.temp-warning{
    color:#fbbf24;
}

.temp-danger{
    color:#ef4444;
}

.current-normal{
    color:#4ade80;
}

.current-warning{
    color:#fbbf24;
}

.current-danger{
    color:#ef4444;
}

.footer{
    text-align:center;
    padding:30px;
    color:#64748b;
    font-size:12px;
}

@media(max-width:600px){

    .header-content{
        flex-direction:column;
        align-items:flex-start;
    }

    .title h1{
        font-size:21px;
    }

    .container{
        padding:15px;
    }

    .metric-value{
        font-size:28px;
    }

}

</style>

</head>

<body>

<header class="header">

<div class="header-content">

<div class="title">

<h1>
Conveyor Belt Monitoring
</h1>

<p>
SIH26008 - Smart Mining Safety System
</p>

</div>

<div class="connection">

<span
class="dot"
id="connectionDot">
</span>

<span id="connectionText">
SYSTEM ONLINE
</span>

</div>

</div>

</header>

<main class="container">

<div
class="alert"
id="alertBox">

<div class="alert-title">
SYSTEM STATUS
</div>

<div
class="alert-value"
id="systemStatus">

Connecting...

</div>

</div>

<div class="grid">

<div class="card">

<div class="card-title">
MOTOR STATUS
</div>

<div class="metric">

<div
class="metric-value"
id="motor">

OFF

</div>

</div>

<div class="status">

<span
class="status-dot">
</span>

<span id="motorStatus">
MOTOR OFF
</span>

</div>

</div>

<div class="card">

<div class="card-title">
MOTOR CURRENT
</div>

<div class="metric">

<div
class="metric-value"
id="current">

0.00

</div>

<div class="metric-unit">
A
</div>

</div>

<div class="status">

<span
class="status-dot">
</span>

<span id="currentStatus">
NORMAL
</span>

</div>

</div>

<div class="card">

<div class="card-title">
MOTOR TEMPERATURE
</div>

<div class="metric">

<div
class="metric-value"
id="temperature">

--

</div>

<div class="metric-unit">
C
</div>

</div>

<div class="status">

<span
class="status-dot">
</span>

<span id="temperatureStatus">
READING
</span>

</div>

</div>

<div class="card">

<div class="card-title">
BELT HEALTH SCORE
</div>

<div class="health-header">

<div
class="health-score"
id="health">

0%

</div>

<div class="status">

<span id="healthState">
NORMAL
</span>

</div>

</div>

</div>

</div>

<div class="card motor-control">

<div class="card-title">
MOTOR CONTROL
</div>

<div class="switch-row">

<div>

<div class="switch-title">
Conveyor Motor
</div>

<div
class="switch-description"
id="motorControlText">

Motor OFF - Ready to Start

</div>

</div>

<label class="switch">

<input
type="checkbox"
id="motorSwitch"
onchange="toggleMotor(this)">

<span class="slider">
</span>

</label>

</div>

</div>

<div class="card health-card">

<div class="health-header">

<div class="card-title">
BELT HEALTH CONDITION
</div>

<div id="healthLabel">
100%
</div>

</div>

<div class="progress">

<div
class="progress-fill"
id="healthBar">
</div>

</div>

</div>

<div class="card info">

<div class="card-title">
LIVE DIAGNOSTICS
</div>

<div class="info-row">

<span class="info-label">
System Message
</span>

<span
class="info-value"
id="message">

---

</span>

</div>

<div class="info-row">

<span class="info-label">
Trip Reason
</span>

<span
class="info-value"
id="trip">

None

</span>

</div>

<div class="info-row">

<span class="info-label">
Temperature Sensor
</span>

<span
class="info-value"
id="sensor">

Checking...

</span>

</div>

</div>

<div class="controls">

<button
class="stop"
onclick="emergencyStop()">

EMERGENCY STOP

</button>

<button
class="reset"
onclick="resetSystem()">

RESET SYSTEM

</button>

</div>

</main>

<div class="footer">

ESP32 Embedded Web Server

<br>

Conveyor Belt Damage Monitoring and Prediction

<br>

SIH26008

</div>

<script>

// ==========================================================
// UPDATE DASHBOARD
// ==========================================================

function updateDashboard()
{

    fetch(
        "/telemetry",
        {
            cache:"no-store"
        }
    )

    .then(
        response =>
        {

            if(!response.ok)
            {
                throw new Error("Network error");
            }

            return response.json();

        }
    )

    .then(
        data =>
        {

            document.getElementById(
                "connectionText"
            ).innerText =
                "SYSTEM ONLINE";

            document.getElementById(
                "connectionDot"
            ).style.background =
                "#4ade80";


            // MOTOR STATUS

            document.getElementById(
                "motor"
            ).innerText =
                data.motor
                ? "RUNNING"
                : "OFF";


            document.getElementById(
                "motorStatus"
            ).innerText =
                data.motor
                ? "MOTOR RUNNING"
                : "MOTOR OFF";


            const motorSwitch =
                document.getElementById(
                    "motorSwitch"
                );

            motorSwitch.checked =
                data.motor;


            document.getElementById(
                "motorControlText"
            ).innerText =
                data.motor
                ? "Motor ON - Conveyor Running"
                : "Motor OFF - Ready to Start";


            // CURRENT

            let current =
                Number(
                    data.current
                );

            document.getElementById(
                "current"
            ).innerText =
                current.toFixed(2);


            const currentElement =
                document.getElementById(
                    "current"
                );

            currentElement.classList.remove(
                "current-normal",
                "current-warning",
                "current-danger"
            );


            if(current < 2)
            {

                currentElement.classList.add(
                    "current-normal"
                );

                document.getElementById(
                    "currentStatus"
                ).innerText =
                    "NORMAL";

            }

            else if(current < 2.5)
            {

                currentElement.classList.add(
                    "current-warning"
                );

                document.getElementById(
                    "currentStatus"
                ).innerText =
                    "WARNING";

            }

            else
            {

                currentElement.classList.add(
                    "current-danger"
                );

                document.getElementById(
                    "currentStatus"
                ).innerText =
                    "CRITICAL";

            }


            // TEMPERATURE

            const temperatureElement =
                document.getElementById(
                    "temperature"
                );

            temperatureElement.classList.remove(
                "temp-normal",
                "temp-warning",
                "temp-danger"
            );


            if(data.temperatureValid)
            {

                let temperature =
                    Number(
                        data.temperature
                    );

                temperatureElement.innerText =
                    temperature.toFixed(2);


                document.getElementById(
                    "temperatureStatus"
                ).innerText =
                    "VALID READING";


                document.getElementById(
                    "sensor"
                ).innerText =
                    "DS18B20 ONLINE";


                if(temperature < 45)
                {

                    temperatureElement
                    .classList
                    .add("temp-normal");

                }

                else if(temperature < 60)
                {

                    temperatureElement
                    .classList
                    .add("temp-warning");

                }

                else
                {

                    temperatureElement
                    .classList
                    .add("temp-danger");

                }

            }

            else
            {

                temperatureElement.innerText =
                    "--";

                document.getElementById(
                    "temperatureStatus"
                ).innerText =
                    "SENSOR ERROR";

                document.getElementById(
                    "sensor"
                ).innerText =
                    "DS18B20 NOT DETECTED";

            }


            // HEALTH

            let health =
                Number(
                    data.health
                );

            document.getElementById(
                "health"
            ).innerText =
                health + "%";

            document.getElementById(
                "healthLabel"
            ).innerText =
                health + "%";

            document.getElementById(
                "healthState"
            ).innerText =
                data.healthState;


            const bar =
                document.getElementById(
                    "healthBar"
                );

            bar.style.width =
                health + "%";


            if(health >= 80)
            {
                bar.style.background =
                    "#22c55e";
            }
            else if(health >= 50)
            {
                bar.style.background =
                    "#f59e0b";
            }
            else
            {
                bar.style.background =
                    "#ef4444";
            }


            // DIAGNOSTICS

            document.getElementById(
                "message"
            ).innerText =
                data.message;

            document.getElementById(
                "trip"
            ).innerText =
                data.tripReason;


            const alertBox =
                document.getElementById(
                    "alertBox"
                );

            alertBox.classList.remove(
                "danger",
                "warning"
            );


            if(
                data.message.includes(
                    "EMERGENCY"
                )
            )
            {
                alertBox.classList.add(
                    "danger"
                );
            }
            else if(health < 80)
            {
                alertBox.classList.add(
                    "warning"
                );
            }


            document.getElementById(
                "systemStatus"
            ).innerText =
                data.message;

        }
    )

    .catch(
        error =>
        {

            console.log(error);

            document.getElementById(
                "connectionText"
            ).innerText =
                "SYSTEM OFFLINE";

            document.getElementById(
                "connectionDot"
            ).style.background =
                "#ef4444";

        }
    );

}

// ==========================================================
// MOTOR SWITCH
// ==========================================================

function toggleMotor(
    switchElement
)
{

    if(
        switchElement.checked
    )
    {

        fetch(
            "/motor/on"
        )

        .then(
            response =>
            {

                if(!response.ok)
                {
                    return response.text()
                    .then(
                        message =>
                        {
                            throw new Error(
                                message
                            );
                        }
                    );
                }

                return response.text();

            }
        )

        .then(
            data =>
            {

                console.log(data);

                updateDashboard();

            }
        )

        .catch(
            error =>
            {

                console.log(error);

                switchElement.checked =
                    false;

                updateDashboard();

                alert(
                    "Motor cannot be started.\n\n" +
                    error.message
                );

            }
        );

    }

    else
    {

        fetch(
            "/motor/off"
        )

        .then(
            response =>
                response.text()
        )

        .then(
            data =>
            {

                console.log(data);

                updateDashboard();

            }
        )

        .catch(
            error =>
            {

                console.log(error);

                updateDashboard();

            }
        );

    }

}

// ==========================================================
// EMERGENCY STOP
// ==========================================================

function emergencyStop()
{

    if(
        confirm(
            "Activate EMERGENCY STOP?"
        )
    )
    {

        fetch(
            "/stop"
        )

        .then(
            response =>
                response.text()
        )

        .then(
            data =>
            {

                console.log(data);

                updateDashboard();

            }
        );

    }

}

// ==========================================================
// RESET
// ==========================================================

function resetSystem()
{

    if(
        confirm(
            "Reset conveyor monitoring system?"
        )
    )
    {

        fetch(
            "/reset"
        )

        .then(
            response =>
            {

                if(!response.ok)
                {
                    return response.text()
                    .then(
                        message =>
                        {
                            throw new Error(
                                message
                            );
                        }
                    );
                }

                return response.text();

            }
        )

        .then(
            data =>
            {

                console.log(data);

                updateDashboard();

            }
        )

        .catch(
            error =>
            {

                alert(
                    "System cannot be reset.\n\n" +
                    error.message
                );

                updateDashboard();

            }
        );

    }

}

// ==========================================================
// INITIAL UPDATE
// ==========================================================

updateDashboard();

// ==========================================================
// UPDATE EVERY 1.5 SECONDS
// ==========================================================

setInterval(
    updateDashboard,
    1500
);

</script>

</body>

</html>

)rawliteral";

// ============================================================
// WEB SERVER ROOT
// ============================================================

void handleRoot()
{
    server.send_P(
        200,
        "text/html",
        dashboardHTML
    );
}

// ============================================================
// LOCAL DASHBOARD TELEMETRY
// ============================================================

void handleTelemetry()
{
    String json = "{";

    json += "\"motor\":";
    json += motorRunning
            ? "true"
            : "false";

    json += ",\"current\":";
    json += String(
        currentValue,
        2
    );

    json += ",\"temperature\":";
    json += String(
        temperatureValue,
        2
    );

    json += ",\"temperatureValid\":";
    json += temperatureValid
            ? "true"
            : "false";

    json += ",\"health\":";
    json += String(
        beltHealth
    );

    json += ",\"healthState\":\"";
    json += getHealthState();
    json += "\"";

    json += ",\"message\":\"";
    json += getSystemMessage();
    json += "\"";

    json += ",\"tripReason\":\"";
    json += tripReason;
    json += "\"";

    json += "}";

    server.send(
        200,
        "application/json",
        json
    );
}

// ============================================================
// MOTOR ON FROM DASHBOARD
// ============================================================

void handleMotorOn()
{
    if(emergencyStop)
    {
        server.send(
            409,
            "text/plain",
            "Emergency Stop Active. Reset system first."
        );

        return;
    }

    if(currentValue >= CURRENT_LIMIT)
    {
        server.send(
            409,
            "text/plain",
            "Overcurrent detected. Motor cannot start."
        );

        return;
    }

    if(
        temperatureValid &&
        temperatureValue >= TEMP_LIMIT
    )
    {
        server.send(
            409,
            "text/plain",
            "High temperature detected. Motor cannot start."
        );

        return;
    }

    motorON();

    tripReason = "None";

    server.send(
        200,
        "text/plain",
        "Motor RUNNING"
    );

    Serial.println(
        "Dashboard command: MOTOR RUNNING"
    );
}

// ============================================================
// MOTOR OFF FROM DASHBOARD
// ============================================================

void handleMotorOff()
{
    motorOFF();

    server.send(
        200,
        "text/plain",
        "Motor OFF"
    );

    Serial.println(
        "Dashboard command: MOTOR OFF"
    );
}

// ============================================================
// EMERGENCY STOP
// ============================================================

void handleStop()
{
    emergencyStop = true;

    tripReason =
        "Manual Emergency Stop";

    motorOFF();

    server.send(
        200,
        "text/plain",
        "Emergency Stop Activated"
    );

    Serial.println(
        "EMERGENCY STOP ACTIVATED"
    );
}

// ============================================================
// RESET
// ============================================================

void handleReset()
{
    if(
        currentValue < CURRENT_LIMIT &&
        (
            !temperatureValid ||
            temperatureValue < TEMP_LIMIT
        )
    )
    {
        emergencyStop = false;

        tripReason = "None";

        motorOFF();

        server.send(
            200,
            "text/plain",
            "System Reset. Motor OFF."
        );

        Serial.println(
            "SYSTEM RESET"
        );

        Serial.println(
            "Motor remains OFF. Use dashboard switch to start."
        );
    }
    else
    {
        server.send(
            409,
            "text/plain",
            "Unsafe condition still present."
        );
    }
}

// ============================================================
// WIFI CONNECTION
// ============================================================

void connectWiFi()
{
    WiFi.mode(
        WIFI_STA
    );

    WiFi.begin(
        ssid,
        password
    );

    Serial.println();

    Serial.println(
        "======================================"
    );

    Serial.println(
        "          WIFI CONNECTION"
    );

    Serial.println(
        "======================================"
    );

    Serial.print(
        "Connecting"
    );

    unsigned long startTime =
        millis();

    while(
        WiFi.status() != WL_CONNECTED &&
        millis() - startTime < 20000
    )
    {
        delay(500);

        Serial.print(".");
    }

    Serial.println();

    if(
        WiFi.status() ==
        WL_CONNECTED
    )
    {
        Serial.println(
            "WIFI CONNECTED!"
        );

        Serial.print(
            "IP Address: "
        );

        Serial.println(
            WiFi.localIP()
        );

        Serial.print(
            "Dashboard: http://"
        );

        Serial.println(
            WiFi.localIP()
        );
    }
    else
    {
        Serial.println(
            "WIFI CONNECTION FAILED!"
        );

        Serial.print(
            "WiFi Status: "
        );

        Serial.println(
            WiFi.status()
        );
    }

    Serial.println(
        "======================================"
    );
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(
        115200
    );

    delay(1500);

    analogReadResolution(
        12
    );

    pinMode(
        RELAY_PIN,
        OUTPUT
    );

    // ALWAYS START MOTOR OFF

    motorOFF();

    // ========================================================
    // DS18B20
    // ========================================================

    temperatureSensor.begin();

    delay(500);

    int sensorCount =
        temperatureSensor.getDeviceCount();

    ds18b20Detected =
        sensorCount > 0;

    Serial.println();

    Serial.println(
        "======================================"
    );

    Serial.println(
        "      CONVEYOR MONITORING SYSTEM"
    );

    Serial.println(
        "======================================"
    );

    Serial.print(
        "DS18B20 devices found: "
    );

    Serial.println(
        sensorCount
    );

    if(ds18b20Detected)
    {
        Serial.println(
            "DS18B20 STATUS: DETECTED"
        );
    }
    else
    {
        Serial.println(
            "DS18B20 STATUS: NOT DETECTED"
        );
    }

    // ========================================================
    // ACS712 CALIBRATION
    // ========================================================

    calibrateACS712();

    // ========================================================
    // WIFI
    // ========================================================

    connectWiFi();

    // ========================================================
    // WEB SERVER ROUTES
    // ========================================================

    server.on(
        "/",
        handleRoot
    );

    server.on(
        "/telemetry",
        handleTelemetry
    );

    server.on(
        "/motor/on",
        handleMotorOn
    );

    server.on(
        "/motor/off",
        handleMotorOff
    );

    server.on(
        "/stop",
        handleStop
    );

    server.on(
        "/reset",
        handleReset
    );

    // ========================================================
    // START SERVER
    // ========================================================

    server.begin();

    Serial.println(
        "Web Server Started!"
    );

    if(
        WiFi.status() ==
        WL_CONNECTED
    )
    {
        Serial.print(
            "OPEN DASHBOARD: http://"
        );

        Serial.println(
            WiFi.localIP()
        );
    }

    Serial.println(
        "======================================"
    );

    Serial.println(
        "SYSTEM READY"
    );

    Serial.println(
        "Motor is OFF."
    );

    Serial.println(
        "Use dashboard switch to start motor."
    );

    Serial.println(
        "======================================"
    );
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
    server.handleClient();

    if(
        millis() - lastSensorRead >= 5000
    )
    {
        lastSensorRead =
            millis();

        currentValue =
            readCurrent();

        readTemperature();

        calculateBeltHealth();

        safetyCheck();

        printStatus();

        sendTelemetry();
    }
}