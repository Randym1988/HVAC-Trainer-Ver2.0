import sys

with open('e:/Randy/HVAC Trainer/src/main.cpp', 'r', encoding='utf-8') as f:
    code = f.read()

bad_str = """if (id == "force_defrost") { force_defrost = state; } } else if (id == "reset_score") {
          student_score = 100;
          work_history_log = "";
          latest_diagnosis = "None";
      else if (id.startsWith("sim_")) {"""

good_str = """if (id == "force_defrost") { force_defrost = state; } 
      else if (id == "reset_score") {
          student_score = 100;
          work_history_log = "";
          latest_diagnosis = "None";
      } else if (id.startsWith("sim_")) {"""

code = code.replace(bad_str, good_str)

with open('e:/Randy/HVAC Trainer/src/main.cpp', 'w', encoding='utf-8') as f:
    f.write(code)

print("Fix applied")