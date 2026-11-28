#pragma once

#include <glib.h>

G_BEGIN_DECLS

//the lines a build with -Dtest_report=true tells the end to end tests what happened with, one JSON object each,
//appended to the file WAYTATOR_TEST_REPORT names. what goes in them is in waytator-window-test-report.c

typedef struct _WaytatorTestReportLine WaytatorTestReportLine;

//NULL while WAYTATOR_TEST_REPORT is unset or empty. every function below takes NULL and does nothing with it
WaytatorTestReportLine *waytator_test_report_line_new(const char *event);
void waytator_test_report_line_add_string(WaytatorTestReportLine *line,
                                          const char             *key,
                                          const char             *value);
void waytator_test_report_line_add_int(WaytatorTestReportLine *line,
                                       const char             *key,
                                       gint64                  value);
//null when the value is infinite or not a number, JSON has neither
void waytator_test_report_line_add_double(WaytatorTestReportLine *line,
                                          const char             *key,
                                          double                  value);
void waytator_test_report_line_add_bool(WaytatorTestReportLine *line,
                                        const char             *key,
                                        gboolean                value);
//an array of numbers, such as a rect
void waytator_test_report_line_add_doubles(WaytatorTestReportLine *line,
                                           const char             *key,
                                           const double           *values,
                                           guint                   n_values);
//what is added from here to the matching end goes into an object of its own under key
void waytator_test_report_line_begin_object(WaytatorTestReportLine *line,
                                            const char             *key);
void waytator_test_report_line_end_object(WaytatorTestReportLine *line);
//appends the line to the file, flushed before this returns, and frees it
void waytator_test_report_line_write(WaytatorTestReportLine *line);

G_END_DECLS
