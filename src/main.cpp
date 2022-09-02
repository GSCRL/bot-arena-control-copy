#include <Arduino.h>
#include <PushButton.h>

/*
** Robot Combat Arena Control Code
** Using Ardunio framework / ESP32 (maybe STM32 with GPIO changes)
** Read 8 buttons
** ready and tap out for 2 teams
** Match start / pause / end /reset for ref
** Serial out to hacked iCruze display boards
** Will control 1 or 2 "stoplight" towers with signal horns
** NF 2022/06/04

*/

// State diagram
/* no match states:
    Reset and ready for start ( solid yellow )
      team a ready
      team b ready
      both teams ready (blinking yellow)
    match pause (blinking yellow or blinking red / match timer paused)
    match tapout (blinking red)
    match end by KO (blinking red)
    match end by time (solid red)
  Match states
    Start pressed (3 secs of blinking yellow followed by green.  Match timer starts then)
    start pressed after pause (same as above with timer resuming)
    active match (solid green)
    10 sec count down (blinking green)
  Horn will sound at match end, time up or team tapout
*/
// inline echo for debug
#define DEBUG_ON 1
#define DEBUG_OFF 0
byte debugMode = DEBUG_ON;

#define DBG(...) debugMode == DEBUG_ON ? Serial.println(__VA_ARGS__) : NULL
//#define DEBUG

// button GPIO's ESP32 / Change for STM32
#define TEAM_A_START 23
#define TEAM_A_END 22
#define TEAM_B_START 33
#define TEAM_B_END 32
#define MATCH_START 25
#define MATCH_PAUSE 26
#define MATCH_END 27
#define MATCH_RESET 14
// tower signal light GPIO's
#define R_LIGHT 2
#define Y_LIGHT 4
#define G_LIGHT 5
#define HORN 13

#define D_SER_TX 17
#define D_SER_RX 16

#define MATCH_LEN 180 // match len in sec ( 3 min)
#define MATCH_END_WARN 15 // ending warn time sec
#define BLINK_DELAY 500 // in ms
#define STARTUP_DELAY 5000 //5 sec
#define MAIN_LOOP_DELAY 5 // in ms
#define HORN_SHORT 1000 //ms
#define HORN_LONG 2000 //ms
#define DIS_DELAY 200 //ms display update rate

PushButton Start_A(TEAM_A_START);
PushButton End_A(TEAM_A_END);
PushButton Start_B(TEAM_B_START);
PushButton End_B(TEAM_B_END);
PushButton GameStart(MATCH_START);
PushButton GameOver(MATCH_END);
PushButton GamePause(MATCH_PAUSE);
PushButton GameReset(MATCH_RESET);

uint64_t Btn_timer;
uint64_t light_timer;
uint64_t Horn_timer;
uint64_t Timer_timer;
uint64_t Display_timer;
uint64_t Dis_min;
uint64_t Dis_sec;

// Match state
enum MatchState{ all_ready, team_a_ready, team_b_ready, starting, in_progress, ending, unpaused, paused, team_a_tap, team_b_tap, time_up, ko_end };
enum MatchState g_match;
enum MatchState last_match_state;
bool g_Match_Reset;

uint64_t gMatchRunTime; //match run time ms
uint64_t gMatchStartTime; //match start time ms
uint64_t MatchSecRemain; // sec remaining
int64_t CountDownMSec; // count down
bool isTimerRunning;
uint64_t gSDtimer; // used for start delay
uint64_t gBLtimer; // for light blinking
uint8_t gBLstate; // for light blinking
uint64_t gTootTimer; //for horn
uint8_t gHornState; // for horn
uint8_t gHornBlast;

// button reading and state setting is done here
void readBtns(MatchState &match, bool &Match_Reset)
{
  // note the button.cycleCount() resets the button press counter
  uint8_t BtnCycle;

  // if match is running these buttons are active
    if ((match == MatchState::in_progress)||(match == MatchState::ending))
    {
      End_A.update();
      End_B.update();
      GameOver.update();
      GamePause.update();
      BtnCycle =GameReset.cycleCount();
      //BtnCycle =GameReset.cycleCount();
      Match_Reset = false;

      if (GamePause.isCycled())
      {
        BtnCycle = GamePause.cycleCount();
        match = MatchState::paused;
      }
      // team tap out
      if ((End_A.isCycled() || End_B.isCycled()) && (match == MatchState::in_progress))
      {
        // Team A has tapped out
        if(End_A.isCycled())
          match = MatchState::team_a_tap;
        // Team B has tapped out
        if(End_B.isCycled())
          match = MatchState::team_b_tap;
        BtnCycle = End_A.cycleCount();
        BtnCycle = End_B.cycleCount();
      }

      // Ref has ended the match
      if(GameOver.isCycled() && (match == MatchState::in_progress)) 
      {
        BtnCycle = GameOver.cycleCount();
        match = MatchState::ko_end;
      }
    }
    else
    {
      // match is ended these are active
      // starting is the match start countdown state
      if ((match != MatchState::starting)&&(match != MatchState::paused)&&(match != MatchState::unpaused))
      {
        {
          GameStart.update();
          Start_A.update();
          Start_B.update();
          GameReset.update();
          // this discards the reads on A, B and main start if not reset
          if (Match_Reset == false)
          {
            BtnCycle = Start_A.cycleCount();
            BtnCycle = Start_B.cycleCount();
            BtnCycle = GameStart.cycleCount();
          }
          if (GameReset.isCycled())
          {
            Match_Reset = true;
            BtnCycle =GameReset.cycleCount();
          }

          // check to see if both teams are ready
          if (Start_A.isCycled() and Start_B.isCycled())
          {
            match = MatchState::all_ready;
            BtnCycle = Start_A.cycleCount();
            BtnCycle = Start_B.cycleCount();
          }
          else
          {
            if (Start_A.isCycled())
            {
              match = MatchState::team_a_ready;
            }
            if (Start_B.isCycled())
            {
              match = MatchState::team_b_ready;
            }
          }
          // if everyone is ready and start is pressed
          if (GameStart.isCycled() and (match == MatchState::all_ready))
          {
            BtnCycle = GameStart.cycleCount();
            BtnCycle = GameReset.cycleCount();
            match = MatchState::starting;
          }
        }
      }
    // match paused
      else
      {
        if(match == MatchState::paused)
        {
          GameStart.update();
          if(GameStart.isCycled())
          {
            match = MatchState::unpaused;
            BtnCycle = GameStart.cycleCount();
          }
        }
      }
    }
}

// used to blink the light tower
void blink(u_int8_t &state, uint64_t &lastBlink, u_int8_t ledGPIO)
{
  if((millis() - lastBlink) > BLINK_DELAY)
  {
    if (state > 0)
    {
      digitalWrite(ledGPIO, HIGH);
      state = 0;
    }
    else
    {
      digitalWrite(ledGPIO, LOW);
      state = 1;
    }
    lastBlink = millis();
  }
}

// handle lights here
void setLights(MatchState &match, bool Match_Reset)
{
  
// all lights off
  if((Match_Reset) && (match != MatchState::starting))
  {
    digitalWrite(G_LIGHT,LOW);
    digitalWrite(Y_LIGHT,LOW);
    digitalWrite(R_LIGHT,LOW);
  }
  // match in progress
  if (match == MatchState::in_progress)
  {
    digitalWrite(G_LIGHT,HIGH);
    digitalWrite(Y_LIGHT,LOW);
    digitalWrite(R_LIGHT,LOW);
  }
  // match is stopped
  if ((match != MatchState::in_progress) && (match != MatchState::starting) && (match != MatchState::ending)&& (!Match_Reset))
  {
    digitalWrite(G_LIGHT,LOW);
    //digitalWrite(Y_LIGHT,LOW);
    digitalWrite(R_LIGHT,HIGH);
  }
  // match is starting
  // blink yellow for x sec, red out, green out
  if(match == MatchState::starting)
  {
    digitalWrite(G_LIGHT,LOW);
    blink(gBLstate, gBLtimer, Y_LIGHT);
    digitalWrite(R_LIGHT,LOW);
  }
  // match is paused
  // red is lit yellow is blinking
  if(match == MatchState::paused)
  {
    blink(gBLstate, gBLtimer, Y_LIGHT);
    digitalWrite(R_LIGHT,HIGH);
  }
  // match is ending (within 15 sec of end)
  // blink green
  if(match == MatchState::ending)
  {
    blink(gBLstate, gBLtimer, G_LIGHT);
  }

  // to do when everything working
  // Team A tap blink A tower red
  // Team B tap blink B tower red
  // for now just blink both towers red
}

// light debug function
void LightDebugPrint(MatchState match, u_int8_t Match_Reset)
{
  switch (match)
  {
  case MatchState::all_ready:
    Serial.println("All ready");
    break;
  case MatchState::team_a_ready:
    Serial.println("team_a_ready");
    break;
  case MatchState::team_b_ready:
    Serial.println("team_b_ready");
    break;
  case MatchState::starting:
    Serial.println("starting");
    break;
  case MatchState::ending:
    Serial.println("ending");
    break;
  case MatchState::unpaused:
    Serial.println("unpaused");
    break;
  case MatchState::paused:
    Serial.println("paused");
    break;
  case MatchState::team_a_tap:
    Serial.println("team_a_tap");
    break;
  case MatchState::team_b_tap:
    Serial.println("team_b_tap");
    break;
  /*case MatchState::time_up:
    Serial.println("time_up");
    break;
  case MatchState::ko_end:
    Serial.println("ko_end");*/
  default:
    break;
  }
}

// used to sound horn (yet another timer)
// not tested
void soundHorn(u_int8_t &hornBlast, uint64_t &hornTime, u_int32_t tootLen, u_int8_t GPIO)
{
  u_int8_t state  = hornBlast % 2;
  if((millis() - hornTime) > tootLen)
  {
    if (state == 1)
    {
      digitalWrite(GPIO, HIGH);
    }
    else
    {
      digitalWrite(GPIO, LOW);
    }
    hornTime = millis();
    hornBlast++;
  }
}

//  match time control
void match_timer(MatchState &l_match, u_int64_t &StartTime, u_int64_t &timerValue, bool &running, bool reset)
{
  // start timer
	if((l_match == MatchState::in_progress)&&(running == false))
  {
      // reset timer at beginning of match
      StartTime = millis();
      timerValue = 0;
      running = true;
  }
  //restart after pause
  if((l_match == MatchState::unpaused)&&(running == false))
  {
      running = true;
      // need to adjust start time to account for pause
      StartTime = millis() - timerValue;
      l_match = MatchState::in_progress;
  }
  //stop timer
  if((l_match != MatchState::in_progress) && (l_match != MatchState::ending))
  {
      // done for debug only
      if(running)
      {
        running = false;
      }
  }
  //update timer
  if((l_match == MatchState::in_progress)||(l_match == MatchState::ending))
  { 
    if ((running) && (timerValue < (MATCH_LEN * 1000)))
    {
      timerValue = millis() - StartTime;
    }
    else
    {
      running = false;
    }
    if(running == false)
    {
      l_match = MatchState::time_up;
    }
  }
	// set match end warning
	if((l_match == MatchState::in_progress)&&(timerValue < (MATCH_END_WARN * 1000)))
		l_match = MatchState::ending;
}

void setup() {
  Serial.begin(115200);
  // give the iCruze attiny time to boot
  delay(1000);
  Serial1.begin(9600,SERIAL_8N1,D_SER_RX,D_SER_TX);
  pinMode(R_LIGHT,OUTPUT);
  pinMode (Y_LIGHT,OUTPUT);
  pinMode (G_LIGHT, OUTPUT);
  delay(100);
  // check lights
  digitalWrite(G_LIGHT,HIGH);
  digitalWrite(Y_LIGHT,HIGH);
  digitalWrite(R_LIGHT,HIGH);
  Serial.println("program starting");
  delay(1000);
  digitalWrite(G_LIGHT,LOW);
  digitalWrite(Y_LIGHT,LOW);
  digitalWrite(R_LIGHT,LOW);
  // check horn
  digitalWrite(HORN,HIGH);
  delay(200);
  digitalWrite(HORN,LOW);
  g_Match_Reset = false;
  isTimerRunning = false;
  gMatchRunTime = 0;
  gMatchStartTime = 0;
  gSDtimer = 0;
  CountDownMSec = 0;
  gBLtimer = millis();
  Btn_timer = millis();
  g_match = MatchState::time_up;
  delay(100);
  Serial1.println("---Display Test----");
  Serial1.println("---Line 2----");
}

void loop() 
{
  
  // Button reading is here
  if ((millis()-Btn_timer) > MAIN_LOOP_DELAY)
  {
    readBtns(g_match, g_Match_Reset);

    // set startup delay - match will be inprogress after this is done
    if ((g_match == MatchState::starting)&&(gSDtimer == 0))
    {
      gSDtimer = millis();
    }
    else if (g_match == MatchState::starting)
    {
      CountDownMSec = STARTUP_DELAY -(millis() - gSDtimer);
      if (CountDownMSec < 5)
      {
        // start match reset count down timer
        g_match = MatchState::in_progress;
        CountDownMSec = 0;
        gSDtimer = 0;
      }
    }
    Btn_timer = millis();
  }

  // handle lights
  #ifdef DEBUG
  if ((millis()-light_timer) > MAIN_LOOP_DELAY + 200)
  {
    LightDebugPrint(g_match, g_Match_Reset);
    light_timer = millis();
  }
  #else
  if ((millis()-light_timer) > MAIN_LOOP_DELAY + 1)
  {
    setLights(g_match, g_Match_Reset);
    light_timer = millis();
  }
  #endif
  // handle horn
  // needs work
  #ifdef DEBUG
  #else
  if ((millis()- Horn_timer) > MAIN_LOOP_DELAY * 2 )
  {
		if(g_match == MatchState::starting)
				gHornBlast = 1;
		// short horn on start or pause
		if(g_match == MatchState::in_progress)
		{
			while (gHornBlast < 2)
				soundHorn(gHornBlast,gTootTimer,HORN_SHORT,HORN);
		}  
		// long horn on match end
		if((g_match == MatchState::ko_end)||(g_match == MatchState::team_a_tap)||(g_match == MatchState::team_b_tap)||(g_match == MatchState::team_b_tap)||(gHornBlast<4))
		{
			gHornBlast++;
			while (gHornBlast < 4)
				soundHorn(gHornBlast,gTootTimer,HORN_LONG,HORN);
		}
		Horn_timer = millis();
  }
  #endif

  // update match timer
  if ((millis() + Timer_timer) > MAIN_LOOP_DELAY)
  {
    match_timer(g_match,gMatchStartTime,gMatchRunTime,isTimerRunning,g_Match_Reset);
    // count down timer display
    MatchSecRemain = MATCH_LEN - (gMatchRunTime / 1000);
    Timer_timer = millis();
  }

  // update char display
  // will send info out via serial
  if ((millis()-Display_timer) > DIS_DELAY)
  {
    // using the 2 x 20 line iCruze display
    // refresh display every 200 ms 
    // display time remaining in min : sec on first line
    // status display on second
    Dis_min = MatchSecRemain / 60;
    Dis_sec = MatchSecRemain % 60;
    if(g_match == MatchState::in_progress)
    {
      Serial1.print(Dis_min);
      Serial1.print(":");
      Serial1.printf("%02d",Dis_sec);
      Serial1.println(" Remaining");
      Serial1.println("Match in Progress");
    }
    else
    {
      if(g_match == MatchState::starting)
      {
        Serial1.print("Starting in: ");
        Serial1.printf("%02d",(CountDownMSec/1000));
        Serial1.println();
        Serial1.println("ALL READY-Starting");
      }
			else
			{
			// this should fix some of the flashing display issues
			// don't update display unless stuff has changed	
				if(g_match != last_match_state)
				{
					if(g_match == MatchState::paused)
					{
						Serial1.print(Dis_min);
						Serial1.print(":");
						Serial1.printf("%02d",Dis_sec);
						Serial1.println(" Remaining");
						Serial1.println("--Paused --");
					}
					if(g_match == MatchState::time_up)
					{
						Serial1.println("--- Time's Up ---");
						Serial1.println("-- MATCH OVER ---");
					}
					if(g_match == MatchState::team_a_tap)
					{
						Serial1.print(Dis_min);
						Serial1.print(":");
						Serial1.printf("%02d",Dis_sec);
						Serial1.println(" -MATCH OVER-");
						Serial1.println("--TEAM A TAPOUT--");
					}
					if(g_match == MatchState::team_b_tap)
					{
						Serial1.print(Dis_min);
						Serial1.print(":");
						Serial1.printf("%02d",Dis_sec);
						Serial1.println(" -MATCH OVER-");
						Serial1.println("--TEAM B TAPOUT--");
					}
					if(g_match == MatchState::ko_end)
					{
						Serial1.print(Dis_min);
						Serial1.print(":");
						Serial1.printf("%02d",Dis_sec);
						Serial1.println(" -MATCH OVER-");
						Serial1.println("-- BY KNOCKOUT --");
					}
					if(g_match == MatchState::all_ready)
					{
						Serial1.println("-- 3 min clock ---");
						Serial1.println("--- ALL READY ---");
					}
					if((g_match != MatchState::all_ready)&&(g_Match_Reset == true)&&(g_match != MatchState::starting))
					{
						Serial1.println("-- WAITING FOR ---");
						Serial1.println("----- READY -----");
					}
					last_match_state = g_match;
				}
			}
    }	
    Display_timer = millis();
  }
}