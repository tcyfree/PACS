import subprocess
import xml.etree.ElementTree as ET


trace_paths = [
    "../../sortedTrace/my1612_0.csv",
    "../../sortedTrace/my1612_2.csv",
    "../../sortedTrace/my1613_1.csv",
    "../../sortedTrace/my1613_6.csv",
]
workload_path = "workload.xml"
command = ["./MQSim", "-i", "ssdconfig.xml", "-w", workload_path]

tree = ET.parse(workload_path)
flow = tree.find("IO_Scenario/IO_Flow_Parameter_Set_Trace_Based")
file_path = flow.find("File_Path")
relay_count = flow.find("Relay_Count")
choose_type = flow.find("Choose_type")

relay_count.text = "2"
choose_type.text = "1"

for trace_path in trace_paths:
    print(f"Running {trace_path}", flush=True)
    file_path.text = trace_path
    tree.write(workload_path)
    subprocess.run(command, check=True, stdin=subprocess.DEVNULL)

print(f"Completed {len(trace_paths)} traces.")
