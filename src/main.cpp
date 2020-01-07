#include "Arduino.h"

/** This demo should demonstrate the majority of required parts for your project
 * It includes:
 *  * Bluetooth serial
 *  * Bluetooth serial input
 *  * Rotational frequency detection
 *  * Synchronous flashing LED
 *  * Time based modification to flashing frequency (as function of rotational frequency)
 *  * Can set frequency, ducty cycle, offset, rotational conversion factor, and more from bluetooth serial
 * 
 * TODO (For you):
 *  1. Change frequeny input to double rather than int
 *  2. Persisting settings to permenant memory when updated
 * 
 * Some interesting links:
 * MMA: https://www.baldengineer.com/measure-pwm-current.html
 * 
 * Frequency from interrupt
 * https://esp32.com/viewtopic.php?t=6533
 * 
 * TODO: Allow choice between fixed/set PWM freq and measured
 * 
 * 
 * 
 **/
#include "BluetoothSerial.h"

#define COOL_PERIOD_SECONDS           400

// Argument type defines
#define ARGUMENT_TYPE_NONE            0
#define ARGUMENT_TYPE_LONG            1
#define ARGUMENT_TYPE_DOUBLE          2
#define ARGUMENT_TYPE_STRING          3

// Defines
// Motor to Zeo rotation conversion factor
#define MOTOR_ZEO_GEARING_FACTOR      4.19
// to move to slower sensor I have multiplied this factor by 14.099
// average motor sensor is 17.019 ms avare head sensor is 239.95ms
// #define MOTOR_ZEO_GEARING_FACTOR      0.29 this is pretty good for the orange belt
// #define MOTOR_ZEO_GEARING_FACTOR      0.275 this is using the elastic band
// The 'pin' the LED is on - In the case of NodeMCU pin2 is the onboard led
#define LED_ONBOARD_PIN               2
#define LED_PIN                       12
// DEVKIT V1 uses pin 23
// #define LED_PIN               23
// The PWM channel for the LED 0 to 15
#define LED_PWM_CHANNEL               0
// PWM resolution in bits
#define LED_PWM_RESOLUTION            8
// PWM inital duty
#define LED_PWM_INITAL_DUTY           5

// Frequency measure
#define FREQ_MEASURE_PIN              19
#define FREQ_MEASURE_TIMER            1
#define FREQ_MEASURE_TIMER_PRESCALAR  80
#define FREQ_MEASURE_TIMER_COUNT_UP   true
#define FREQ_MEASURE_TIMER_PERIOD     FREQ_MEASURE_TIMER_PRESCALAR/F_CPU
#define FREQ_MEASUER_SAMPLE_NUM       128
// this returns a pointer to the hw_timer_t global variable
// 0 = first timer
// 80 is prescaler so 80MHZ divided by 80 = 1MHZ signal ie 0.000001 of a second
// true - counts up

// Bounds within which to consider the previous and current
// freqency the same. Comparing doubles/floats on equality is never going to work...
// Instead treat equality as: abs(a-b) > bound
#define FREQ_COMPARE_BOUNDS           0.0001

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

/** Timer for measuring freq **/
volatile uint64_t StartValue;                     // First interrupt value
bool fAdded = false;
// Our own Ring Buffer
uint8_t ringIndex = 0;
uint64_t myRing[FREQ_MEASUER_SAMPLE_NUM] = {0};
// average freq intermediate values as globals. Bite me!
uint64_t sumPeriod;
double avgPeriod;
// prev freq for freq compare
double prevFreq = 0.0;

hw_timer_t * fTimer = NULL;                       // pointer to a variable of type hw_timer_t 
portMUX_TYPE fTimerMux = portMUX_INITIALIZER_UNLOCKED;  // synchs between maon cose and interrupt?


// Digital Event Interrupt
// Enters on falling edge in this example
//=======================================
void IRAM_ATTR handleFrequencyMeasureInterrupt()
{
  portENTER_CRITICAL_ISR(&fTimerMux);
      // uint8_t blah = 8;
      // value of timer at interrupt
      uint64_t TempVal= timerRead(fTimer);
      if (ringIndex == FREQ_MEASUER_SAMPLE_NUM -1 ) {
        ringIndex = 0;
      } else {
        ringIndex++;
      }
      // // Add period to RunningAverage, period is in number of FREQ_MEASURE_TIMER_PERIOD
      // // Note: Is timer overflow safe
      // This one MOFO's
      myRing[ringIndex]= TempVal - StartValue;
      // frequencyRA.addValue(TempVal - StartValue);
      // // puts latest reading as start for next calculation
      StartValue = TempVal;
      fAdded = true;
  portEXIT_CRITICAL_ISR(&fTimerMux);
}

// Create a BluetoothSerial thingee
BluetoothSerial SerialBT;

// Some variables to use in our program - global scope
String serialBuffer = "";
String messages;

// A 'struct' is an object containing other variables
// This defines the struct data type
struct ProgramVars {
  double  pwmFreq;
  long    setFreq;
  bool    useSetFreq;
  long    pwmDutyThou;
  double  freqDelta;
  bool    runVariableDelta;
  double  freqConversionFactor;
  bool    ledEnable;
  bool    logging;
  bool    stateChange;
  String  randomString;
};
// This creates a new variable which is of the above struct type
ProgramVars programVars = {
  0,      // pwmFreq
  0,      // setFreq
  false,  // useSetFreq
  LED_PWM_INITAL_DUTY,      // pwmDutyThou
  1.0,    // freqDelta
  true,    // runVariableDelta
  MOTOR_ZEO_GEARING_FACTOR, // freqConversionFactor
  true,   // ledEnable
  false,  //  logging
  false,  // stateChange
  ""      //randomString
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
    } else if (inputString == "0") {
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

  /** The standard OP for getting/setting/displaying command and args
   * There are two semi identical forms of this function, one where the 
   * argument is a number (long), and one where it is a string
   **/
  boolean argDisplayOrSetDoubleFromLong(String argName, CommandAndArguments comAndArg, double *var, uint16_t denominator, String *message) {
    if (comAndArg.argType == ARGUMENT_TYPE_NONE) {
      *message = argName + " is : " + String(*var);
      return false;
    }
    if (comAndArg.argType == ARGUMENT_TYPE_LONG) {
      *var = 1.0 * comAndArg.argLong / denominator;
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
        String("'f': PWM frequency in HZ\n") +
        String("'p': Whether to measure frequency or use frequency set by 'p'\n") +
        String("'d': PWM duty cycle 0-reolution max (ie 255 for 8 bit)\n") +
        String("'m': Frequency modifier to apply to measured frequency as percentage\n") +
        String("'v': Run variable delta programme Enable (1), or disable (0)\n") +
        String("'r': Rotational gearing ratio * 1000 \n") +
        String("'l': Enable (1), or disable (0) led\n") +
        String("'L': Enable (1), or disable (0) logging");
      break;
    case 'f':
      progVars->stateChange = argDisplayOrSetLong("useSetFreq", comArgState, &progVars->setFreq, message);
      break;
    case 'p':
      progVars->stateChange = argDisplayOrSetBoolean("useSetFreq", comArgState, &progVars->useSetFreq, message);
      break;
    case 'd':
      progVars->stateChange = argDisplayOrSetLong("pwmDuty", comArgState, &progVars->pwmDutyThou, message);
      break;
    case 'm':
      progVars->stateChange = argDisplayOrSetDoubleFromLong("freqDelta", comArgState, &progVars->freqDelta, 100, message);
      break;
    case 'v':
      progVars->stateChange = argDisplayOrSetBoolean("runVariableDelta", comArgState, &progVars->runVariableDelta, message);
      break;
    case 'r':
      progVars->stateChange = argDisplayOrSetDoubleFromLong("freqConversionFactor", comArgState, &progVars->freqConversionFactor, 1000, message);
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
    " setFreq: " + String(progVars.setFreq) +
    " pwmFreq: " + String(progVars.pwmFreq) +
    " pwmDuty: " + String(progVars.pwmDutyThou) +
    " freqDelta: " + String(progVars.freqDelta) +
    " pwmDuty: " + String(progVars.pwmDutyThou) +
    " Random string: '" + progVars.randomString +"'";
}

double calculateFinalFrequency(double avgPeriod, double conversionFactor) {
  double frequencyAtMotor = 1 / (avgPeriod * FREQ_MEASURE_TIMER_PERIOD);
  // Apply the conversion factor
  return frequencyAtMotor * conversionFactor;
}

/** This function modifies the delta/modifier based on the timestamp in seconds
 * on a cycle (hence the modulo)
 **/
void makeShitCoolAgain(uint32_t timestamp, ProgramVars *programVars) {
  uint32_t relativeTime = timestamp % COOL_PERIOD_SECONDS;

  switch(relativeTime) {
    case 0:
      programVars->freqDelta = 1.0;
      break;
    case 1:
      programVars->freqDelta = 1.0;
      break;
    case 102:
      programVars->freqDelta = 1.1;
      break;
    case 103:
      programVars->freqDelta = 1.2;
      break;
    case 104:
      programVars->freqDelta = 1.3;
      break;
    case 105:
      programVars->freqDelta = 1.4;
      break;
    case 106:
      programVars->freqDelta = 1.5;
      break;
    case 107:
      programVars->freqDelta = 1.6;
      break;
    case 108:
      programVars->freqDelta = 1.7;
      break;
    case 109:
      programVars->freqDelta = 1.8;
      break;
    case 110:
      programVars->freqDelta = 1.9;
      break;
    case 111:
      programVars->freqDelta = 2.0;
      break;
    case 112:
      programVars->freqDelta = 2.0;
      break;
    case 113:
      programVars->freqDelta = 2.0;
      break;
    case 114:
      programVars->freqDelta = 2.0;
      break;
    case 115:
      programVars->freqDelta = 2.0;
      break;
    case 116:
      programVars->freqDelta = 2.0;
      break;
    case 117:
      programVars->freqDelta = 2.0;
      break;
    case 118:
      programVars->freqDelta = 2.0;
      break;
    case 119:
      programVars->freqDelta = 2.0;
      break;
    case 120:
      programVars->freqDelta = 2.0;
      break;
    case 121:
      programVars->freqDelta = 2.0;
      break;
    case 122:
      programVars->freqDelta = 2.0;
      break;
    case 123:
      programVars->freqDelta = 2.0;
      break;
    case 124:
      programVars->freqDelta = 2.0;
      break;
    case 125:
      programVars->freqDelta = 2.0;
      break;
    case 126:
      programVars->freqDelta = 2.0;
      break;
    case 127:
      programVars->freqDelta = 2.0;
      break;
    case 128:
      programVars->freqDelta = 2.0;
      break;
    case 129:
      programVars->freqDelta = 2.0;
      break;
    case 130:
      programVars->freqDelta = 2.1;
      break;
    case 131:
      programVars->freqDelta = 2.2;
      break;
    case 132:
      programVars->freqDelta = 2.3;
      break;
    case 133:
      programVars->freqDelta = 2.4;
      break;
    case 134:
      programVars->freqDelta = 2.5;
      break;
    case 135:
      programVars->freqDelta = 2.6;
      break;
    case 136:
      programVars->freqDelta = 2.7;
      break;
    case 137:
      programVars->freqDelta = 2.8;
      break;
    case 138:
      programVars->freqDelta = 2.9;
      break;
    case 139:
      programVars->freqDelta = 3.0;
      break;
    case 140:
      programVars->freqDelta = 3.0;
      break;
    case 141:
      programVars->freqDelta = 3.0;
      break;
    case 142:
      programVars->freqDelta = 3.0;
      break;
    case 143:
      programVars->freqDelta = 3.0;
      break;
    case 144:
      programVars->freqDelta = 3.0;
      break;
    case 145:
      programVars->freqDelta = 3.0;
      break;
    case 146:
      programVars->freqDelta = 3.0;
      break;
    case 147:
      programVars->freqDelta = 3.0;
      break;
    case 148:
      programVars->freqDelta = 3.0;
      break;
    case 149:
      programVars->freqDelta = 3.0;
      break;
    case 150:
      programVars->freqDelta = 3.0;
      break;
    case 151:
      programVars->freqDelta = 3.1;
      break;
    case 152:
      programVars->freqDelta = 3.2;
      break;
    case 153:
      programVars->freqDelta = 3.3;
      break;
    case 154:
      programVars->freqDelta = 3.3;
      break;
    case 155:
      programVars->freqDelta = 3.4;
      break;
    case 156:
      programVars->freqDelta = 3.5;
      break;
    case 157:
      programVars->freqDelta = 3.6;
      break;
    case 158:
      programVars->freqDelta = 3.7;
      break;
    case 159:
      programVars->freqDelta = 3.8;
      break;
    case 160:
      programVars->freqDelta = 3.9;
      break;
    case 161:
      programVars->freqDelta = 4.0;
      break;
    case 162:
      programVars->freqDelta = 4.0;
      break;
    case 163:
      programVars->freqDelta = 4.0;
      break;
    case 164:
      programVars->freqDelta = 4.0;
      break;
    case 165:
      programVars->freqDelta = 4.0;
      break;
    case 166:
      programVars->freqDelta = 4.0;
      break;
    case 167:
      programVars->freqDelta = 4.0;
      break;
    case 168:
      programVars->freqDelta = 4.0;
      break;
    case 169:
      programVars->freqDelta = 4.0;
      break;
    case 170:
      programVars->freqDelta = 4.0;
      break;
    case 171:
      programVars->freqDelta = 4.0;
      break;
    case 172:
      programVars->freqDelta = 4.0;
      break;
    case 173:
      programVars->freqDelta = 4.0;
      break;
    case 174:
      programVars->freqDelta = 4.0;
      break;
    case 175:
      programVars->freqDelta = 4.0;
      break;
    case 176:
      programVars->freqDelta = 4.0;
      break;
    case 177:
      programVars->freqDelta = 4.0;
      break;
    case 178:
      programVars->freqDelta = 4.0;
      break;
    case 179:
      programVars->freqDelta = 4.0;
      break;
    case 180:
      programVars->freqDelta = 4.0;
      break;
    case 181:
      programVars->freqDelta = 3.9;
      break;
    case 182:
      programVars->freqDelta = 3.8;
      break;
    case 183:
      programVars->freqDelta = 3.5;
      break;
    case 184:
      programVars->freqDelta = 3.2;
      break;
    case 185:
      programVars->freqDelta = 3.0;
      break;
    case 186:
      programVars->freqDelta = 2.9;
      break;
    case 187:
      programVars->freqDelta = 2.7;
      break;
    case 188:
      programVars->freqDelta = 2.4;
      break;
    case 189:
      programVars->freqDelta = 2.3;
      break;
    case 190:
      programVars->freqDelta = 2.2;
      break;
    case 191:
      programVars->freqDelta = 2.1;
      break;
    case 192:
      programVars->freqDelta = 1.9;
      break;
    case 193:
      programVars->freqDelta = 1.8;
      break;
    case 194:
      programVars->freqDelta = 1.7;
      break;
    case 195:
      programVars->freqDelta = 1.6;
      break;
    case 196:
      programVars->freqDelta = 1.5;
      break;
    case 197:
      programVars->freqDelta = 1.4;
      break;
    case 198:
      programVars->freqDelta = 1.3;
      break;
    case 199:
      programVars->freqDelta = 1.2;
      break;
    case 200:
      programVars->freqDelta = 1.1;
      break;
    case 210:
      programVars->freqDelta = 1.0;
      break;
    case 215:
      programVars->freqDelta = 1.95;
      break;
    case 220:
      programVars->freqDelta = 1.9;
      break;
    case 230:
      programVars->freqDelta = 1.8;
      break;
    case 235:
      programVars->freqDelta = 1.7;
      break;
    case 240:
      programVars->freqDelta = 1.6;
      break;
    case 245:
      programVars->freqDelta = 1.5;
      break;
    case 250:
      programVars->freqDelta = 1.4;
      break;
    case 260:
      programVars->freqDelta = 1.3;
      break;
    case 270:
      programVars->freqDelta = 1.2;
      break;
    case 280:
      programVars->freqDelta = 1.1;
      break;
    case 290:
      programVars->freqDelta = 1.05;
      break;
    case 300:
      programVars->freqDelta = 1.01;
      break;
    case 330:
      programVars->freqDelta = 1;
      break;
    case 340:
      programVars->freqDelta = 5;
      break;
      case 350:
      programVars->freqDelta = 1;
      break;
      case 360:
      programVars->freqDelta = 5;
      break;
      case 370:
      programVars->freqDelta = 1;
    // OK, lets try something crazy!
    // So you want a sine wave... alg..
    // We want the sine wave to complete in the given time, so
    // 120-90 = 30 must be mapped to 360.. i.e x12
    // But this only happens evey second... damn...
    // I would recommend you put this in a _way_ faster for smoother transitions.
    // There is a bit of process control overhaul that is needed anyway.
    // OR, make the transitions really slow, then good as...
    // REDACTED in merge, but you _still can_ do this 8)
    // case 90 ... 120:
    //   programVars->freqDelta = sin(
    //     (relativeTime-90)*12
    //   );
    //   break;
  }
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
  ledcAttachPin(LED_ONBOARD_PIN, LED_PWM_CHANNEL);
  ledcAttachPin(LED_PIN, LED_PWM_CHANNEL);


  // Setup frequency measure timer
  // sets pin high
  pinMode(FREQ_MEASURE_PIN, INPUT);
  // attaches pin to interrupt on Falling Edge
  attachInterrupt(digitalPinToInterrupt(FREQ_MEASURE_PIN), handleFrequencyMeasureInterrupt, FALLING);
  // Setup the timer
  fTimer = timerBegin(FREQ_MEASURE_TIMER, FREQ_MEASURE_TIMER_PRESCALAR, FREQ_MEASURE_TIMER_COUNT_UP);
  // Start the timer
  timerStart(fTimer);
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

    if (programVars.stateChange == true || fAdded == true) {
      // reset c flhangeag
      fAdded = false;
      programVars.stateChange = false;


      if (programVars.useSetFreq) {
        programVars.pwmFreq = programVars.setFreq;
      } else {
        // calculate the frequency from the average period
        sumPeriod = 0;
        for (int i = 0; i < FREQ_MEASUER_SAMPLE_NUM; ++i)
        {
            sumPeriod += myRing[i];
        }
        avgPeriod = ((double)sumPeriod)/FREQ_MEASUER_SAMPLE_NUM; //or cast sum to double before division
        programVars.pwmFreq = calculateFinalFrequency(avgPeriod, programVars.freqConversionFactor) * programVars.freqDelta;
      }

      messages = "Setting PWM duty to: " + String(programVars.pwmDutyThou) + \
        " Frequency to: " + String(programVars.pwmFreq) + \
        " User set freq to: " + String(programVars.setFreq);

      Serial.println(messages);
      SerialBT.println(messages);
      // We need to change duty to 0 if LED is disabled
      if (programVars.ledEnable == true) {
        ledcWrite(LED_PWM_CHANNEL, programVars.pwmDutyThou);
      } else {
        messages = "Disabling LED";
        ledcWrite(LED_PWM_CHANNEL, 0);
      }
    }

    // Timer fires every quarter second, so every four tickes
    // we increment the timestamp and log
    if (timestampQuarter%4 == 0) {
      timestamp++;
      timestampQuarter = 0;

      if (programVars.runVariableDelta == true) {
        makeShitCoolAgain(timestamp, &programVars);
      }

      // Change the PWM freq if it has changed within the bounds
      if ( abs(programVars.pwmFreq - prevFreq) > FREQ_COMPARE_BOUNDS) {
        ledcWriteTone(LED_PWM_CHANNEL, programVars.pwmFreq);
        prevFreq = programVars.pwmFreq;
        if (programVars.ledEnable == true) {
          ledcWrite(LED_PWM_CHANNEL, programVars.pwmDutyThou);
        } else {
          ledcWrite(LED_PWM_CHANNEL, 0);
        }
      }

      // print logging info if enabled
      if (programVars.logging == true) {
        String logMessage = formatProgVars(timestamp, programVars);    
        Serial.println(logMessage);
        SerialBT.println(logMessage);
      }
    }
  }

  // Do realtime things
  // Calculate the frequency
  // programVars.pwmFreq = programVars.useSetFreq;
  // if (programVars.useSetFreq) {
  //   programVars.pwmFreq = programVars.useSetFreq;
  // } else if (fAdded == true) {
  //   programVars.pwmFreq = calculateFinalFrequency(frequencyRA, programVars.freqConversionFactor) + programVars.freqDelta;
  // }


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
  // ooooo, gross! Don't do that. naa fk ya
  // delay(50);
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

