#pragma once

typedef struct {
    int second;
    int minute;
    int hour;
    int day_of_week;
    int day;
    int month;
    int year;
} ds3231_time_t;

typedef struct {
    int unused;
} ds3231_dev_t;
