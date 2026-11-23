from optparse import Values
import subprocess
import xml.etree.ElementTree as ET

# 指定XML文件路径
xml_file = './workload.xml'

# 解析XML文件
tree = ET.parse(xml_file)

File_path = tree.find("IO_Scenario/IO_Flow_Parameter_Set_Trace_Based/File_Path")
Choose_type = tree.find("IO_Scenario/IO_Flow_Parameter_Set_Trace_Based/Choose_type")
Relay_Count = tree.find("IO_Scenario/IO_Flow_Parameter_Set_Trace_Based/Relay_Count")


Values = [  


            "../../sortedTrace/my1612_0.csv",
            "../../sortedTrace/my1612_2.csv",


            "../../sortedTrace/my1613_1.csv",
            "../../sortedTrace/my1613_6.csv",

            # "../../sortedTrace/my1614_6zip10.csv",

            "../../sortedTrace/my1615_6zip10.csv",

             "../../sortedTrace/mymds_0zip10.csv",

             "../../sortedTrace/my206zip50",

            "../../aliyun72hou/myAli_72h_858zip40", 

            ]
Choose_types = [
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1"
                ]
RelayCounts = [
              2,
              2,
              2,
              2,
              2,
              2,
              2,
              2,
              2,
              2,
              2,
              2,
              2,
              2
              ]
exe_path = './MQsim'
exe_args =['-i','ssdconfig.xml','-w','workload.xml']
cmd = [exe_path]+exe_args

for value, relaycounts,choose_type in zip(Values,RelayCounts,Choose_types):
    File_path.text = value
    Relay_Count.text = str(relaycounts)
    Choose_type.text = choose_type
    tree.write('workload.xml')
    proc = subprocess.Popen(cmd)
    proc.wait()


print(f'所有实例已启动')
