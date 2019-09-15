#include "Arduino.h"

/** This demo should demonstrate the majority of required parts for your project
 * It includes:
 *  * Bluetooth serial
 *  * Bluetooth serial input
 *  * Flashing LED
 *  * Can set frequncy and ducty cycle from bluetooth serial
 * TODO (For you):
 *  1. Persisting settings to permenant memory when updated
 *  2. Inclusion of RPM sensing code
 * 
 * Some interesting links:
 * MMA: https://www.baldengineer.com/measure-pwm-current.html
 * 
 **/
#include "BluetoothSerial.h"

//Timers and counters and things
/** Timer and process control **/
uint32_t timestamp = 0;
int timestampQuarter = 0;
hw_timer_t * timer = NULL;
volatile SemaphoreHandle_t timerSemaphore;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR onTimer(){
  // Increment the counter and set the time of ISR
  portENTER_CRITICAL_ISR(&timerMux);
  timestampQuarter++;
  portEXIT_CRITICAL_ISR(&timerMux);
  // Give a semaphore that we can check in the loop
  xSemaphoreGiveFromISR(timerSemaphore, NULL);
  // It is safe to use digitalRead/Write here if you want to toggle an output
}

// Argument type defines
#define ARGUMENT_TYPE_NONE    0
#define ARGUMENT_TYPE_LONG    1
#define ARGUMENT_TYPE_STRING  2
 

// Defines
// The 'pin' the LED is on - In the case of NodeMCU pin2 is the onboard led
#define LED_PIN               2
// DEVKIT V1 uses pin 23
// #define LED_PIN               23
// The PWM channel for the LED 0 to 15
#define LED_PWM_CHANNEL       0
// PWM resolution in bits
#define LED_PWM_RESOLUTION    8

// Create a BluetoothSerial thingee
BluetoothSerial SerialBT;

// Some variables to use in our program - global scope
String serialBuffer = "";
String messages;

// A 'struct' is an object containing other variables
// This defines the struct data type
struct ProgramVars {
  long pwmFreq;
  long pwmDutyThou;
  bool ledEnable;
  bool logging;
  bool stateChange;
  String randomString;
};
// This creates a new variable which is of the above struct type
ProgramVars programVars = {
  0, 0, true, false, false, ""
};


/** This function takes a string and separates out the command and argument
 * The command is the first character, the argument is the remainder
 * 
 * In C/C++ rather than return the parts, we act on pointers to the variables
 * Pointers to variables (the memory location of a variable) are denoted by a '*'
 * 
 * This function returns EXIT_FAILURE (cuold be 0) if it fails,
 * or EXIT_SUCCESS (could be 1) if everything is OK
 **/
 int getCommandAndArgument(String inputString, char *command, String *argument) {
   // Check that the String is long enough to process and return fail otherwise
   if (inputString.length() <= 1) {
     return EXIT_FAILURE;
   }

   *command = inputString.charAt(0);
   *argument = inputString.substring(1);
   return EXIT_SUCCESS;
 }

 /** stringToLong function that returns state
  * Why is state important?
  * The standard string to integer functions the integer representation
  * of a string if one exists and 0 if it does not (i.e atoi('a') = 0)
  * This is **shit** because "0" is a valid and common number
  * If the string cannot be converted then we should know about it.
  * 
  * To do this we use a function that returns a state and modifes a
  * pointer to an int if the conversion is successful
  * 
  **/
  int stringToLong(String inputString, long *targetInt) {
    // convert the input string to an integer
    int32_t intTemp = inputString.toInt();
    // If the resulting integer is not 0 then no problem
    if (intTemp != 0) {
      *targetInt = intTemp;
      return EXIT_SUCCESS;
    // If the input string is literally "0" no problem
    } else if ( inputString == "0") {
      *targetInt = 0;
      return EXIT_SUCCESS;
    // Otherwise there was a problem
    } else {
      return EXIT_FAILURE;
    }    
  }

  // A 'struct' to hold parsed commands and arguments
  struct CommandAndArguments {
    char    command;
    int     argType;
    long    argLong;
    String  argString;
    boolean parseState;
  };

  /** Parse the command/args string
   * Find the command if present, and parse the arguments
   * determining where there are none, are a number, or a string
   **/
  CommandAndArguments parseCommandArgs(String commandArgs) {
    char comChar = 'h';
    int argType = ARGUMENT_TYPE_NONE;
    long argLong = 0;
    String argString = "";


    // Trim the result, include removing the trailing '/n'
    commandArgs.trim();

    // Check that the String is long enough to process and return fail otherwise
    if (commandArgs.length() == 0) {
      return CommandAndArguments{
        comChar, argType, argLong, argString, EXIT_FAILURE
      };
    } 
    // Get the command
    comChar = commandArgs.charAt(0);

    // If there are enough characters in 'commandArgs' get and parse them
    if (commandArgs.length() > 1) {
      // Separate the argument from the command
      argString = commandArgs.substring(1);
      // If we can convert the argString to a number we do
      if (stringToLong(argString, &argLong) == EXIT_SUCCESS) {
        argType = ARGUMENT_TYPE_LONG;
      } else {
        argType = ARGUMENT_TYPE_STRING;
      }
    }

    // Return all the things
    return CommandAndArguments{
      comChar, argType, argLong, argString, EXIT_SUCCESS
    };
  }

  /** The standard OP for getting/setting/displaying command and args
   * There are two semi identical forms of this function, one where the 
   * argument is a number (long), and one where it is a string
   **/
  boolean argDisplayOrSetLong(String argName, CommandAndArguments comAndArg, long *var, String *message) {
    if (comAndArg.argType == ARGUMENT_TYPE_NONE) {
      *message = argName + " is : " + String(*var);
      return false;
    }
    if (comAndArg.argType == ARGUMENT_TYPE_LONG) {
      *var = comAndArg.argLong;
      *message = "Set '" + argName + "' to : " + String(*var);
      return true;
    }
    return false;
  }
  // String version
  boolean argDisplayOrSetString(String argName, CommandAndArguments comAndArg, String *var, String *message) {
    if (comAndArg.argType == ARGUMENT_TYPE_NONE) {
      *message = argName + " is : '" + *var + "'";
      return false;
    }
    if (comAndArg.argType == ARGUMENT_TYPE_STRING) {
      *var = comAndArg.argString;
      *message = "Set '" + argName + "' to : '" + *var + "'";
      return true;
    }
    return false;
  }
  // Boolean version
  boolean argDisplayOrSetBoolean(String argName, CommandAndArguments comAndArg, boolean *var, String *message) {
    if (comAndArg.argType == ARGUMENT_TYPE_NONE) {
      *message = argName + " is : '" + String(*var) + "'";
      return false;
    }
    // Check if true both string and Long
    comAndArg.argString.toLowerCase();
    if (
        // String and equals 'true'
        (comAndArg.argType == ARGUMENT_TYPE_STRING && comAndArg.argString == "true") ||
        // Long and equals 1
        (comAndArg.argType == ARGUMENT_TYPE_LONG && comAndArg.argLong == 1)
      ) {
        *var = true;
        *message = "Set '" + argName + "' to : 'true'";
        return true;
    }
    // Check if false both string and Long
    if (
        // String and equals 'true'
        (comAndArg.argType == ARGUMENT_TYPE_STRING && comAndArg.argString == "false") ||
        // Long and equals 1
        (comAndArg.argType == ARGUMENT_TYPE_LONG && comAndArg.argLong == 0)
      ) {
        *var = false;
        *message = "Set '" + argName + "' to : 'false'";
        return true;
    }
    return false;
  }

 /** *do things* based on inputString:
  *   * Update progVars
  *   * Show progVars
  * This function should not change output state, but if vars change
  * the 'stateChange' flag shoudl be set
  *  
  **/
  int processCommands(String inputString, ProgramVars *progVars, String *message) {
    // Parse the 'inputString'
    CommandAndArguments comArgState = parseCommandArgs(inputString);

    // Exit with message if no command
    if (comArgState.parseState == EXIT_FAILURE) {
      *message = "Input string is not a valid command/argument";
      return EXIT_FAILURE;
    }

    // Let us process the commands
    switch (comArgState.command)
    {
    case 'h':
      progVars->stateChange = false;
      *message = String("Help: \n") + 
        String("Commands will return current value if no argument given, and set to value if given\n") +
        String("'p': PWM frequency in HZ\n") +
        String("'d': PWM duty cycle 0-reolution max (ie 255 for 8 bit)\n") +
        String("'l': Enable (1), or disable (0) logging");
      break;
    case 'p':
      progVars->stateChange = argDisplayOrSetLong("pwmFreq", comArgState, &progVars->pwmFreq, message);
      break;
    case 'd':
      progVars->stateChange = argDisplayOrSetLong("pwmDuty", comArgState, &progVars->pwmDutyThou, message);
      break;
    case 's':
      progVars->stateChange = argDisplayOrSetString("randomString", comArgState, &progVars->randomString, message);
      break;
    case 'l':
      progVars->stateChange = argDisplayOrSetBoolean("ledEnable", comArgState, &progVars->ledEnable, message);
      break;
    case 'L':
      progVars->stateChange = argDisplayOrSetBoolean("logging", comArgState, &progVars->logging, message);
      break;
    default:
      progVars->stateChange = false;
      *message = "No recognised command";
      break;
    }
    return EXIT_SUCCESS;
  }

String formatProgVars(long time, ProgramVars progVars) {
  return String(time) + " ledEnable: " + String(progVars.ledEnable) +
    " pwmFreq: " + String(progVars.pwmFreq) +
    " pwmDuty: " + String(progVars.pwmDutyThou) +
    " Random string: '" + progVars.randomString +"'";
}
  

void setup() {
  // Initialise the serial hardware at baude 115200
  Serial.begin(115200);
 
  // Initialise the Bluetooth hardware with a name 'ESP32'
  if(!SerialBT.begin("ESP32")){
    Serial.println("An error occurred initializing Bluetooth");
  }

  // Give a semaphore that we can check in the loop
  // Setup Timer 1 on interrupt
  // Create semaphore to inform us when the timer has fired
  timerSemaphore = xSemaphoreCreateBinary();
  // Use 1st timer of 4 (counted from zero).
  // Set 80 divider for prescaler (see ESP32 Technical Reference Manual for more
  // info).
  timer = timerBegin(0, 80, true);
  // Attach onTimer function to our timer.
  timerAttachInterrupt(timer, &onTimer, true);
  // Set alarm to call onTimer function every quarter second (value in microseconds).
  // Repeat the alarm (third parameter)
  timerAlarmWrite(timer, 1000000/4, true);
  // Start an alarm
  timerAlarmEnable(timer);


  // Attach an LED thingee
  // configure LED PWM functionalitites
  // ledcSetup(LED_PWM_CHANNEL, programVars.pwmFreq, LED_PWM_RESOLUTION);
  ledcSetup(LED_PWM_CHANNEL, 500, LED_PWM_RESOLUTION);
  // attach the channel to the GPIO to be controlled
  ledcAttachPin(LED_PIN, LED_PWM_CHANNEL);
}
 
void loop() {
 
  // If Timer has fired do some non-realtime stuff
  if (xSemaphoreTake(timerSemaphore, 0) == pdTRUE){
    // If the buffers end in newline, try to parse the command an arguments
    if(serialBuffer.endsWith("\n")) {
      // Print out the buffer - for fun
      Serial.println(serialBuffer);
      // Process the commands
      processCommands(serialBuffer, &programVars, &messages);
      // Print the message
      Serial.println(messages);
      SerialBT.println(messages);
      // Reset the buffer to empty
      serialBuffer = "";
    }

    if (programVars.stateChange == true) {
      messages = "Setting PWM duty to: " + String(programVars.pwmDutyThou) + \
        " Frequency to: " + String(programVars.pwmFreq);
      Serial.println(messages);
      SerialBT.println(messages);
      ledcWriteTone(LED_PWM_CHANNEL, programVars.pwmFreq);
      // We need to change duty to 0 if LED is disabled
      if (programVars.ledEnable == true) {
        ledcWrite(LED_PWM_CHANNEL, programVars.pwmDutyThou);
      } else {
        messages = "Disabling LED";
        ledcWrite(LED_PWM_CHANNEL, 0);
      }
      programVars.stateChange = false;
    }

    // Timer fires every quarter second, so every four tickes
    // we increment the timestamp and log
    if (timestampQuarter%4 == 0) {
      timestamp++;
      timestampQuarter = 0;

      // print logging info if enabled
      if (programVars.logging == true) {
        String logMessage = formatProgVars(timestamp, programVars);    
        Serial.println(logMessage);
        SerialBT.println(logMessage);
      }
    }
  }

  // Do realtime things
  // While there are characters in the Serial buffer
  // read them in one at a time into sBuffer
  // We need to go via char probably due to implicit type conversions
  while(Serial.available() > 0){
    char inChar = Serial.read();
    serialBuffer += inChar;
  }
 
  // As above but with the bluetooth device
  while(SerialBT.available() > 0){
    char inChar = SerialBT.read();
    serialBuffer += inChar;
  }
 
  // Wait 50 ms
  delay(50);
}

// #include "TimerOne.h"

// const int numreadings = 5;
// volatile unsigned long readings[numreadings];
// int index = 0;
// unsigned long average = 0;
// unsigned long period1 = 0;
// unsigned long total=0;
//
// volatile unsigned block_timer = 0;
// volatile bool block=false;
//
// volatile bool calcnow = false;
//
// //Strobe settings
// const int strobePin = 9;
// float dutyCycle = 5.0;  //percent
// float dutyCycle_use = (dutyCycle / 100) * 1023;
// float strobe_period_float = 0;
// unsigned long period_microsec = 0;
// const unsigned long non_strobe_period = 1000; //stobe period when not yet in strobe mode - just a light. 1000us == 1000hz
//
// float stobes_per_rot = 11.;
//
// void rps_counter(); // forward declare
//
//
// void rps_counter(){ /* this code will be executed every time the interrupt 0 (pin2) gets low.*/
//   //this is really a bit much code for an ISR, but we only have ~3 rps so it should be ok
//
//   if(block==false){
//     readings[index] = micros();
//     index++;
//
//     //Double triggering block
//     block_timer = micros();
//     block=true;
//
//     if(index >= numreadings){
//      index=0;
//      calcnow=true;
//     }
//   } //if "blocked", do nothing
// }
//
// void setup() {
//    Serial.begin(115200);
//    //initial light mode
//    Timer1.initialize(non_strobe_period);  // 10 000 000 us = 10 Hz
//    Timer1.pwm(strobePin, dutyCycle_use);
//
//    attachInterrupt(0, rps_counter, FALLING); //interrupt 0 is pin 2
// }
//
//
// void loop(){
//
//   //Check hall effect block timer
//   if(block){
//     // detachInterrupt(0);    //Disable interrupt for ignoring multitriggers
//     if(micros()-block_timer >= 100000){
//       block=false;
//       // attachInterrupt(0, rps_counter, FALLING); //enable interrupt
//     }
//   }
//
//   if(calcnow){
//     // detachInterrupt(0);    //Disable interrupt when printing
//     total = 0;
//     for (int x=numreadings-1; x>=1; x--){ //count DOWN though the array, every 70 min this might make one glitch as the timer rolls over
//       period1 = readings[x]-readings[x-1];
//       total = total + period1;
//     }
//     average = total / (numreadings-1); //average period
//
//     Serial.println(average);
//
//     if(average<10000000){ //ie, more than 0.1hz of rotation.
//       strobe_period_float = average / stobes_per_rot ;
//       period_microsec = (unsigned int) strobe_period_float;
//       Timer1.setPeriod(period_microsec);
//       Timer1.pwm(strobePin, dutyCycle_use);
//     }
//     else{
//       Serial.println("ack too slow, going to constant light");
//       Timer1.setPeriod(non_strobe_period);
//       Timer1.pwm(strobePin, dutyCycle_use);
//     }
//     calcnow = false;
//     // attachInterrupt(0, rps_counter, FALLING); //enable interrupt
//   }
// }
