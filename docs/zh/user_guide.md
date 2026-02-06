# 用户指南
## 1. 测试框架

### 1.1 功能测试

本项目采用Google Test框架对KDADK-APPID中实现的方法函数进行测试，测试用例的数据来自于采集到的pcap流量文件。在进行测试前，需要将pcap流量文件放在KDADK-APPID项目的./test/traces文件夹下供测试时使用。

测试流程：

1. 首先运行KDADK-APPID项目下的build.sh生成动态链接库.so文件供测试使用。

```
sh build.sh -lib
```

2. 进入test文件夹下的build目录。

```
cd test/build
```

3. 编译功能测试代码

```
make
```

4. 运行功能测试

```
make rfunc
```

### 1.2 性能测试

测试流程：

1. 首先运行KDADK-APPID项目下的build.sh生成动态链接库.so文件供测试使用。

```
sh build.sh -lib
```

2. 进入test文件夹下的build目录。

```
cd test/build
```

3. 编译功能测试代码

```
make
```

4. 运行功能测试

```
make rperf
```