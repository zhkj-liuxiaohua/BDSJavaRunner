# BDSJavaRunner

#### 介绍
提供Win版MC服务端下Java插件运行平台（源自player技术支持）

#### 软件架构
采用JSONCPP开源框架

#### 使用方式
1. 安装JRE8(64位版本)。
2. 配置文件javasetting.ini放入BDS所在目录下，其中jvmpath定义了JVM虚拟机所在位置，jardir定义了JAR插件库所在目录。
3. 放置JAR插件(后缀名为.bds.jar)至JAR插件库目录下加载即可。

如何编译：
解压工程目录中 MCBDS插件开发助手.zip ，使用工具导出MC服务端对应的pdb文件 RVAs.hpp 替换原工程文件即可。

如何开发：
1. 创建新的java工程，复制包BDS下的MCJAVAAPI模板至源码目录下；
2. 在您自行构建的类的main函数中传递arg[0], arg[1], arg[2]进行MCJAVAAPI类的构造即可使用BDS相关监听器和API。
3. 打包您的java工程为可执行jar程序，修改后缀为.bds.jar即可通过BDSJavaRunner进行带参数加载。

#### Commercial License
LGPL