#include <SD.h>
#include <math.h>
#include <SPI.h>
#include <SD.h>

#define DOOR_ALTITUDE 95
#define TARGET_TEMP 30
#define TEMP_TOLERANCE 1
#define USB_BAUD 9600
#define LKM_DEFAULT_BAUD 9600
#define LKM_STARTUP_TIME 120000

#define BAD_CMD -1
#define BAD_VAL -2
#define MISMATCH -3
#define BAD_STAT -4

#define PWR_INDEX  0
#define PULS_INDEX 1
#define DATA_INDEX 2
#define VOLT_INDEX 3
#define PRES_INDEX 4
#define TEMP_INDEX 5
#define MOTR_INDEX 6

// Pins, hardware
// Burn Door Deploy 
int BDD = 4;
// thermal control
int heater = 6;
// Chip select for SD card
int CS = 7;
// Thermistors
int therm1 = A3;
int therm2 = A4;
// Indicator LEDs
int SDinitLED = 8;
int LKMcommLED = 9;
int altitudeCalibratedLED = 10;
int readingThermistorsLED = 11;
int allSystemsLED = 12;
int launchedLED = 13;
int launchSwitch = 2;

// Global variables
float initPressure;
unsigned long lastPacemaker;
unsigned long lastGroundComm;
unsigned long launchTime;
unsigned long lastTempControl = 0;
unsigned long timeEnteredAutonomous;
bool autonomousMode = 0;
bool tempConcern = 0;
bool tempFreakOut = 0;
bool launched = 0;
bool doorDeployed = 0;
float PID_kP = .5;
float PID_kI = .1;
float PID_kD = 0;

// Other global variables
int pacemakerPeriod = 60000;
int groundCommPeriod = 60000;
int burnTime = 300000;
int doorTimeout;
int LKMsetupTimeout;
char delim = ',';
char terminator = ';';

// global arrays to store telem 
// [PWR, PULS, DATA, VOLT, PRES, TEMP, MOTR]
enum Command {PWR, PULS, DATA, VOLT, PRES, STAT, MOTR, KP, KI, KD};
float expected_val[] = {1.0, 0.0, 9600, 5.0, 1000.0, 27.0, 30.0};
float stats[7];

// Temp control stuff
class averagingArray{
  public:
  static const int arraySize = 100;
  int data[arraySize];
  int iEnd = 0;
  long arraySum = 0;
  averagingArray(){
    for(int i = 0; i < arraySize; i++){
      data[i] = 0;
    }
  }
  long pop(){
    long foundValue = data[iEnd];
    arraySum -= foundValue;
    return foundValue;
  }
  
  long addNew(long newData){
    long prevVal = -1;
    // Update the end
    iEnd++;
    iEnd = iEnd % arraySize;
    
    // Pop off the old value, add the new one
    prevVal = pop();
    data[iEnd] = newData;
    arraySum += newData;
    return prevVal;
  }
  
  float average(){
    // This is supposed to return a float, but it will return an int...
    return arraySum / arraySize;
  }

  int lastVal(){
    return data[iEnd];
  }

  int lastDeltaY(){
    return data[iEnd] - data[(iEnd - 1 + arraySize) % arraySize];
  }
  
};

averagingArray data;

void setup() {
  // Set up indicator LEDs
  pinMode(SDinitLED, OUTPUT);
  pinMode(LKMcommLED, OUTPUT);
  pinMode(altitudeCalibratedLED, OUTPUT);
  pinMode(readingThermistorsLED, OUTPUT);
  pinMode(allSystemsLED, OUTPUT);
  pinMode(launchedLED, OUTPUT);
  
  // Initialize UARTs 
  Serial.begin(9600);
  Serial1.begin(LKM_DEFAULT_BAUD);
  
  // Initialize SD card, 5 min timeout
  bool SDinit = initializeSDcard(300000);
  if(SDinit){
    digitalWrite(SDinitLED, HIGH);
  }
  // Make sure we're talking to the LKM
  // Delay long enough for the LKM to start up
  delay(LKM_STARTUP_TIME);
  // Change baud rate
  // Maybe we should check if it's done starting up first?
  Serial1.readStringUntil("\n");
  
  lowerBaudRate(2400);
  bool LKMcomm = 0;
  unsigned long startLKMtestTime = millis();
  while(! LKMcomm && (millis() - startLKMtestTime < LKMsetupTimeout)){
    LKMcomm = checkLKMcomm(10);
  }
  if(LKMcomm){
    digitalWrite(LKMcommLED, HIGH);
  }
  // Calibrate the current altitude to 0 +- 5, averaging over 3 readings
  bool altitudeCalibrated = calibrateAltitude(3, 5);
  if(altitudeCalibrated){
    digitalWrite(altitudeCalibratedLED, HIGH);
  }
  bool readingThermistors = checkThermistor(therm1, -20, 50) && checkThermistor(therm2, -20, 50); 
  if(readingThermistors){
    digitalWrite(readingThermistorsLED, HIGH);
  }

  // Turn the heater off
  Serial1.print("$PULS, 0;");
  parseLKM();
  
  // Make sure all the systems we've checked are okay
  // I'm making this its own bool so that if we decide we want to do something (like run again
  // or whatever, we can just grab the allSystems bool
  bool allSystems = SDinit && LKMcomm && altitudeCalibrated && readingThermistors;
  if(allSystems){
    digitalWrite(allSystemsLED, HIGH);
  }
}

void loop() {
  if(! launched && digitalRead(launchSwitch)){
    launchTime = millis();
    launched = true;
    digitalWrite(launchedLED, HIGH);
  }
  pacemakerIfNeeded(pacemakerPeriod);
  burnIfNeeded(doorTimeout);
  controlTemps(TARGET_TEMP, TEMP_TOLERANCE);
  handleGroundCommand();
  if(millis() - lastGroundComm > groundCommPeriod){
    autonomousMode = true;
    Serial.print("Yo talk to us");
    recordVitals("Entered autonomous mode");
    timeEnteredAutonomous = millis();
  }
  while(autonomousMode && launched){
    pacemakerIfNeeded(pacemakerPeriod);
    burnIfNeeded(doorTimeout);
    controlTemps(TARGET_TEMP, TEMP_TOLERANCE);
    Serial.print("Pls I'm lonely");
    Serial.print("Entered autonomous mode ");
    Serial.print((millis() - timeEnteredAutonomous) / 1000);
    Serial.print(" seconds ago");
    if(handleGroundCommand()){
      autonomousMode = false;
    }
  }
}


// Helper functions
void recordVitals(String event){
  Serial1.write("$STAT;");
  String telem = "";
  delay(100);
  if (Serial1.available()) {
    telem = Serial1.readStringUntil(terminator);
  }
  
  // open the file.
  File dataFile = SD.open("datalog.txt", FILE_WRITE);

  // if the file is available, write the time and vitals to it:
  if (dataFile) {
    dataFile.println(millis() + ',' + telem + ',' + event);
    dataFile.close();
  }
  Serial.print("Event: ");
  Serial.print(event);
}

boolean pacemakerIfNeeded(int pacemakerPeriod){
  if(millis() - lastPacemaker > pacemakerPeriod){
    recordVitals("");
    return 1;
  }
  return 0;
}

boolean burnIfNeeded(int timeout){
  // Only check if we need to burn if the door hasn't already been deployed
  if(! doorDeployed){
    if(findAltitude() > DOOR_ALTITUDE || millis() - launchTime > timeout){
      burnWire();
      doorDeployed = 1;
    }
  }
}

void burnWire(){
  recordVitals("Door: burn started");
  digitalWrite(BDD, HIGH);
  delay(burnTime);
  digitalWrite(BDD, LOW);
  recordVitals("Door: deployed");
  
}

void controlTemps(float target, float err_tolerance){
  // Should use Klesh's function to get temp
  // which temp reading am i supposed to use??? we doing temp of LKM processor?
  float temp = readThermistor(therm1);
  Serial.write("$TEMP;");
  if (Serial.available()) {
    Serial.readStringUntil(delim); // should be #TEMP,
    temp = Serial.readStringUntil(terminator).toFloat();
  }
  float error = target - temp;
  data.addNew(error);
  // prolly need to somehow record time elapsed between thermistor readings
  // also need to record error into data structure 
  // record any temperatures outside of a tolerable range
  if (error > err_tolerance) {
    recordVitals("WARNING: temp too low");
  }
  else if (error < -err_tolerance) {
    recordVitals("WARNING: temp too high");
  }
  //need to tune this as well... oh boy 
  analogWrite(heater, 255.0 / 100.0 * PID(PID_kP, PID_kI, PID_kD)); 
  lastTempControl = millis();
}


float findAltitude(){
  /*This is a vaguely sketchy formula that I got from
   * https://keisan.casio.com/has10/SpecExec.cgi?id=system/2006/1224585971
   * and then proceded to make the assumption that we could change our reference point to the launch 
   * altitude (instead of sea level) by using our starting pressure as Po
   */
  float pressure;
  Serial.write("$PRES;");
  if (Serial.available()) {
    Serial.readStringUntil(delim); // should be #PRES,
    pressure = Serial.readStringUntil(terminator).toFloat();
  }
  
  // This is assuming the external temp is 15C; we should probably add an extra thermistor to check this
  // make sure pressure is in hPa?
  // this returns height in meters... will need to convert to feet
  float temp = 15.0;
  float h = (pow((initPressure/pressure), 1.0/5.257) - 1) * (temp + 273.15) / .0065;
  return h;
}

bool calibrateAltitude(int nTimes, int altitudeError){
  float pressure = 0;
  for(int i = 0; i < nTimes; i++){
    Serial.write("$PRES;");
    delay(100);
    if(Serial.available()){
      Serial.readStringUntil(delim); //should be #PRES,
      pressure += Serial.readStringUntil(terminator).toFloat();
    }
  }
  // Take the average pressure
  initPressure = pressure / nTimes;
  // We probably can't check that the measured altitude is exactly 0.00...
  if(initPressure > 0 && -altitudeError < findAltitude() && findAltitude() < altitudeError){
    // Probably add a (generous) expected range once we know units, etc
    return 1;
  } else {
    return 0;
  }
}

bool initializeSDcard(int timeout){
  SD.begin(CS);
  // Wait for it to initialize
  while(!SD.begin(CS)){
    if(millis() > timeout){
      return 0;
    } else {
      return 1;
    }
  }
}

bool checkLKMcomm(int nTries){
  for(int i = 0; i < nTries; i++){
    Serial1.write("$STAT;");
    if(parseStat() == 1.0){
      return 1;
    }
    delay(100);
  }
  return 0;
}

bool checkThermistor(int thermistorPin, int lowerBound, int upperBound){
  float temp = readThermistor(thermistorPin);
  if(temp > lowerBound && temp < upperBound){
    return 1;
  } else {
    return 0;
  }
}

float readThermistor(int thermistorPin) {
  // function to read the temperature
  // input: int thermistorPin is an analog pin number (eg A0) 
  // output: double representing the temperature in Celsius
  
  // thermistorPin needs to be an analog pin
  // Connect the thermistor in series with another resistor if value Ro, with thermistor closer to ground
  float Ro = 10000.0;
  // Using Vcc = 5V
  float Vcc = 5;
  // analogRead returns an int in [0, 1023], so it needs to be scaled to volts
  float reading = analogRead(thermistorPin);
  float Vthermistor = (5 / 1023.0) * reading;
  // Now for the real question, can I use Ohm's law?
  float Rthermistor = Vthermistor * Ro / (Vcc - Vthermistor);

  // Using some model with some values I found in the NTCLE100E3 data sheet
  // number 9
  float A = 3.354016 * pow(10,-3);
  float B = 2.569850 * pow(10,-4);
  float C = 2.620131 * pow(10,-6);
  float D = 6.383091 * pow(10,-8);
  float Rref = 10000.0;
  float temp = 1.0 / (A + B * log( Rref/Rthermistor) + C * pow(log( Rref/Rthermistor), 2) + D * pow(log( Rref/Rthermistor), 3));
  // calibration factor of 18 and K -> C
  temp = temp - 273;
  // C -> F
  //temp = 9/5 * temp + 32;
  return temp; // in Celsius
}

double PID(float Pcoeff, float Icoeff, float Dcoeff){
 float proportionalControl = Pcoeff * data.lastVal();
 float integralControl = Icoeff * data.average();
 float derivativeControl = Dcoeff * data.lastDeltaY() / (millis() - lastTempControl);
 float controlRaw = proportionalControl + integralControl + derivativeControl;
 return max(min(controlRaw, 1), 0) * 100;
//  return 0.0;
}

bool lowerBaudRate(int baud){
  // Assume we're communicating with the LKM at 9600 to start
  // Lower baud rate using $DATA
  Serial1.write("$DATA, ");
  Serial1.write(baud);
  Serial1.write(";");
  delay(100);
  bool error = 0;
  while(Serial1.available()){
    // If there's stuff on the serial, read it so it doesn't just sit in the buffer and note an error
    // Because $DATA shouldn't return anything, so if 
    Serial1.read();
    error = 1;
  }
  if(error){
    // try one more time
    Serial1.write("$DATA, ");
    Serial1.write(baud);
    Serial1.write(";");
    delay(100);
    if(! Serial1.available()){
      error = 0;
    }
    while(Serial1.available()){
      Serial1.read();
    }
  }
  if(error){
    return 0;
  }
  Serial1.begin(baud);
  // try n times, in case there's an error
  // possible that this will eventually get built into checkLKMcomm
  int nAttempts = 3;
  for(int i = 0; i < nAttempts; i++){
    if(checkLKMcomm(2)){
      return 1;
    }
  }
  return 0;
  // If failure, should probably communicate with ground too?
}


float parseLKM(){
  delay(100);
  String cmd, val;
  //read back in format #cmd,val;
  if(Serial.available()) {
    cmd = Serial.readStringUntil(delim);
    val = Serial.readStringUntil(terminator);
  }

  // go through each command value pair and return the value or error
  return processCmdVal(cmd, val, false);
}



float processCmdVal(String cmd, String val, boolean save) {

  // go through each command value pair and return the value or error
  if (cmd.equals("#PWR") ){
    if(val.equals("ON")){
      val = "1.0";
    }
    else if (val.equals("OFF")) {
      val = "0.0";
    }
    else {
      return BAD_VAL;
    }
    return processTelem(val, PWR_INDEX, save);
  }
  else if (cmd.equals("#PULS") || cmd.equals("PULS") ){
    return processTelem(val, PULS_INDEX, save);
  }
  else if (cmd.equals("#DATA") || cmd.equals("DATA") ){
    return processTelem(val, DATA_INDEX, save);
  }
  else if (cmd.equals("#VOLT") || cmd.equals("VOLT") ){
    return processTelem(val, VOLT_INDEX, save);
  }
  else if (cmd.equals("#PRES") || cmd.equals("PRES") ){
    return processTelem(val, PRES_INDEX, save);
  }
  else if (cmd.equals("#TEMP") || cmd.equals("TEMP") ){
    return processTelem(val, TEMP_INDEX, save);
  }
  else if(cmd.equals("#MOTR") || cmd.equals("MOTR") ){
    return processTelem(val, MOTR_INDEX, save);
  }
  else {
    return BAD_CMD;
  }
}



float processTelem(String val, int index, bool saveStat) {
  float output;
  output = isValidTelem(val);
  if (saveStat) {
    stats[index] = output;
  }
  if (output == BAD_VAL) {
    return BAD_VAL;
  }
  else if (output == expected_val[index]){
    return output;
  }
  else if (index == VOLT_INDEX || index == PRES_INDEX || index == TEMP_INDEX) {
    expected_val[index] = output; // update expected_val
    return output;
  }
  else {
    return MISMATCH;
  }
}


float isValidTelem(String str){
  boolean isNum=false;
  for(byte i=0;i<str.length();i++)
  {
    if (i == 0) {
      isNum = isDigit(str.charAt(i)) || str.charAt(i) == '.' || str.charAt(i) == '-';
    }
    else {
      isNum = isDigit(str.charAt(i)) || str.charAt(i) == '.' ;
    }
    if(!isNum) {
      return -2.0;
    }
  }
  return str.toFloat();
}



bool handleGroundCommand(){
  if(!Serial.available()){
    return false;
  }
  lastGroundComm = millis();
  String command = Serial.readStringUntil(delim);
  bool sendToLKM = 0;
  int dataIndex;
  if(command.equals("PWR")){
      sendToLKM = 1;
      dataIndex = PWR_INDEX;
  }
  if(command.equals("PULS")){
      sendToLKM = 1;
      dataIndex = PULS_INDEX;
  }
  if(command.equals("DATA")){
      sendToLKM = 1;
      dataIndex = DATA_INDEX;
  }
  if(command.equals("VOLT")){
      sendToLKM = 1;
      dataIndex = VOLT_INDEX;
  }
  if(command.equals("PRES")){
      sendToLKM = 1;
      dataIndex = PRES_INDEX;
  }
  if(command.equals("STAT")){
      sendToLKM = 1;
  }
  if(command.equals("MOTR")){
      sendToLKM = 1;
      dataIndex = MOTR_INDEX;
  }
  if(command.equals("KP")){
    // how to do error checking here?
    // TODO
      PID_kP = Serial.readStringUntil(terminator).toFloat();
   }
   if(command.equals("KI")){
      PID_kI = Serial.readStringUntil(terminator).toFloat();
   }
   if(command.equals("KD")){
      PID_kD = Serial.readStringUntil(terminator).toFloat();
   }
    else{
      // Complain to ground
      Serial.write("Whatcha say?");
   }
  
  if(sendToLKM){
    // Send command
    Serial1.write("$");
    String commandToSend = "Hi";
    Serial1.print(command);
    // Add the comma back
    Serial1.write(delim);
    // Send all the args
    while(Serial.available()){
      // it's probably not actually necesssary to do this, could probably read all
      // But if there's two commands in a row stored in the UART that feels a little weird
      Serial1.print(Serial.readStringUntil(terminator));
    }
    Serial1.write(terminator);
  }
  recordVitals("Ground command: " + command);
  return true;
}


float parseStat(){
  delay(100);
  String stat;
  //read back stat;
  if(Serial.available()) {
    stat = Serial.readStringUntil(terminator);
    Serial.println(stat);
  }
  
  int start = 0;
  int nextIndex = 0;
  String cmd, val;
  float result;
  boolean stat_error = false;
  
  // go through stat and process the cmd,val pairs
  while (nextIndex != -1) {
    nextIndex = stat.indexOf(',', start);
//    Serial.print("next val start: ");
//    Serial.println(nextIndex);
    if (nextIndex != -1) {
      cmd = stat.substring(start, nextIndex);
//      Serial.print("cmd: ");
//      Serial.println(cmd);
      start = nextIndex + 1;

      nextIndex = stat.indexOf(',', start);
//      Serial.print("next cmd start: ");
//      Serial.println(nextIndex);
      if (nextIndex != -1) {
        val = stat.substring(start, nextIndex);
//        Serial.print("val: ");
//        Serial.println(val);
      }
      else{
         val = stat.substring(start);
//         Serial.print("val: ");
//         Serial.println(val);
      }
      
    }
    else {
      stat_error = true;
    }
    start = nextIndex + 1;

    result = processCmdVal(cmd, val, true);
    if (result == -1 || result == -2) {
      stat_error = true;
    }
    
  }// end while
  
  if (stat_error) {
    return BAD_STAT;
  }
  return 1.0;
}
