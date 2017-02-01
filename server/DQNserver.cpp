// based on RasPiRH95.cpp contributed by Alan Marchiori and Mike Poublon


#include <wiringPi.h>

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>


#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string.h>
#include <RH_RF95.h>

#include "../core/protocol.h"

// DQN related constants
const uint32_t FEEDBACK_TIME = (LORA_HEADER + DQN_PREAMBLE + sizeof(struct dqn_feedback)) * 8000 / DQN_RATE;
const uint32_t TR_TIME = (LORA_HEADER + DQN_PREAMBLE + sizeof(struct dqn_tr)) * 8000 / DQN_RATE; // take into packet header into account


//Function Definitions
void sig_handler(int sig);
void print_feedback(struct dqn_feedback fb);
void print_packet(uint8_t *data, int length);


// pin numers use wiringPi numbers.
#define RF95_RESET_PIN 0  // this is BCM pin 17, physical pin 11.
#define RF95_INT_PIN 7    // this is BCM pin 4, physical pin 7.
#define RF95_CS_PIN 10    // this is BCM pin 8, physical pin 24

// wiringPi pin numbers
#define TX_PIN 4
#define RX_PIN 5

RH_RF95 rf95(RF95_CS_PIN, RF95_INT_PIN);

//Flag for Ctrl-C
volatile sig_atomic_t flag = 0;

//Main Function
int main (int argc, const char* argv[] ){
    signal(SIGINT, sig_handler);

    wiringPiSetup();

    printf( "\nFeedback delay %d ms\n\n", FEEDBACK_TIME);

    /* Begin Driver Only Init Code */
    pinMode(RF95_RESET_PIN, OUTPUT);
    pinMode(TX_PIN, OUTPUT);
    pinMode(RX_PIN, OUTPUT);
    digitalWrite(TX_PIN, HIGH);
    digitalWrite(RX_PIN, HIGH);

    digitalWrite(RF95_RESET_PIN, HIGH);
    delay(50);
    digitalWrite(RF95_RESET_PIN, LOW);
    delay(50);
    digitalWrite(RF95_RESET_PIN, HIGH);
    delay(50);

    printf("Reset high, waiting 1 sec.\n");
    delay(1000);

    digitalWrite(TX_PIN, LOW);
    digitalWrite(RX_PIN, LOW);

    if (!rf95.init()){
        printf("rf95 init failed.\n");
        exit(-95);
    }else{
        printf("rf95 init success.\n");
    }
    if (!rf95.setFrequency (915.0)){
        printf("rf95 set freq failed.\n");
        exit(-96);
    }else{
        printf("rf95 set freq to %5.2f.\n", 915.0);
    }

    if (rf95.setModemConfig(rf95.Bw500Cr48Sf4096)){
        printf("rf95 configuration set to BW=500 kHz BW, CR=4/8 CR, SF=12.\n");
    }else{
        printf("rf95 configuration failed.\n");
        exit(-97);
    }

    // set the preamble
    rf95.setPreambleLength(DQN_PREAMBLE);
    printf("rf95 set preamble to %d\n", DQN_PREAMBLE);

    rf95.setTxPower(23, false);

    uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];

    // parameters used by DQN
    uint16_t crq = 0;
    uint16_t dtq = 0;

    // TODO: decouple them into multiple functions
    printf("DQN mini slot frame size %d mini slot size: %d DQN overhead: %d TR time: %d\n", 
            DQN_MINI_SLOT_FRAME, DQN_MINI_SLOT_LENGTH, DQN_OVERHEAD, TR_TIME);

    while (true){
        uint32_t frame_start = millis();

        uint16_t new_crq = crq;
        uint16_t new_dtq = dtq;

        // setup TR counter
        uint8_t tr_results[DQN_M];
        for(uint8_t i = 0; i < DQN_M; i++){
            tr_results[i] = 0;
        }
        const uint32_t CYCLE_START_TIME = millis();
        // wait for mini slot requests
        // TODO: need to adjust this time based on how fast
        // the packet can transmit
        while(millis() < CYCLE_START_TIME + DQN_MINI_SLOT_FRAME){
            if(rf95.available()){
                uint32_t received_time = millis();
                printf("got a TR packet\n");
                uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
                uint8_t len = sizeof(buf);
                if (rf95.recv(buf, &len)){
                    struct dqn_tr* tr = (struct dqn_tr *)buf;
                    uint8_t crc = tr->crc;
                    tr->crc = 0;
                    uint8_t packet_crc = get_crc8((char*)tr, sizeof(struct dqn_tr));
                    // calculate the slot number
                    uint32_t time_offset = received_time - CYCLE_START_TIME - TR_TIME;
                    
                    // heuristic fix the correctness of time_offset due to clock issues
                    if(time_offset < 0 and time_offset + DQN_GUARD > 0)
                        time_offset = 0;

                    uint8_t mini_slot = time_offset / DQN_MINI_SLOT_LENGTH;
                    uint32_t slot_time_offset = time_offset % DQN_MINI_SLOT_LENGTH;
                    printf("offset: %d requested mini slot %d offset %d\n", time_offset, 
                            mini_slot, time_offset % DQN_MINI_SLOT_LENGTH);
                    // set the status of the mini slot 
                    if(packet_crc == crc){
                        tr_results[mini_slot] = tr->num_slots;
                        new_dtq += tr->num_slots;
                    } else{
                        printf("actual requested: %d\n", tr->num_slots);
                        tr_results[mini_slot] = DQN_N;
                        new_crq += 1;
                    }
                }
            }
        }

        // immediately send the feedback result 
        // so that TR and feedback will be put into the same channel frequency later on
        struct dqn_feedback feedback;
        feedback.crq_length = crq;
        feedback.dtq_length = dtq;
        // process the mini slots
        for(int i = 0; i < DQN_M; i++){
            //uint8_t result = 0;
            //for(int j = 0; j < 4; j++){
            //    // each status only needs 2 bits
            //    uint8_t status = tr_results[j + i * 4];
            //    // adding crq and dtq accordingly
            //    result |= status << (3 - j); // lower address to high address
            //}
            // TODO: compress the space
            feedback.slots[i] = tr_results[i];
        }

        // handle crc
        feedback.crc = 0;
        feedback.crc = get_crc8((char*)&feedback, sizeof(feedback));
        // send the feedback
        if(!rf95.send((uint8_t *)&feedback, sizeof(feedback))){
            printf("sending feedback failed");
        } else{
            printf("sent feedback...\n");
            print_feedback(feedback);
        }

        // change back to high transmission mode
        //

        // reduce the queue length
        dtq = new_dtq > DQN_N? new_dtq - DQN_N: 0;
        crq = new_crq > 1? new_crq - 1: 0;


        // moved to the receive window
        // DQN_LENGTH ms for overhead 
        delay(DQN_LENGTH * DQN_OVERHEAD - (millis() - CYCLE_START_TIME));

        while(millis() < CYCLE_START_TIME + DQN_LENGTH * (DQN_OVERHEAD + DQN_N)){
            // TODO: add channel hopping
            if(rf95.available()){
                // TODO: assemble the fragment together and return to the library user
                uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
                uint8_t len = sizeof(buf);
                if (rf95.recv(buf, &len))
                {
                    printf("receiving data... size %d\n", len);
                    print_packet(buf, len);
                } else{
                    printf("receiving failed\n");
                }
            }
        }

        if (flag)
        {
            printf("\n---CTRL-C Caught - Exiting---\n");
            break;
        }

        printf("CYCLE is %d\n", millis() - frame_start);
    }

    return 0;
}

void sig_handler(int sig)
{
    if (flag == 1){
        printf("\n--- Double CTRL-L - panic stop---\n");
        exit(-99);
    }
    flag=1;
}

void print_feedback(struct dqn_feedback fb){
    for(int i = 0; i < DQN_M; i++){
        printf("[%d]: %d\t", i, fb.slots[i]);
    }
    printf(" CRQ: %d\tDTQ: %d\n", fb.crq_length, fb.dtq_length);
}

void print_packet(uint8_t *data, int length){
    //const int PRINT_SIZE = 32;
    //length = length > PRINT_SIZE? PRINT_SIZE:length;
    for(int i = 0; i < length; i++){
        printf("%02X ", data[i]);
        if(i % 16 == 15)
            printf("\n");
    }
    printf("\n");
}
