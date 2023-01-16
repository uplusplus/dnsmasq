# dnsmasq ndk编译

## 准备
安装ndk r21e

## 编译
```
ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk
```



## 关于dhcp6

### 简介
DHCPv6是一个用来配置工作在IPv6网络上的IPv6主机所需的IP地址、IP前缀和/或其他配置的网络协议。
IPv6主机可以使用无状态地址自动配置（SLAAC）或DHCPv6来获得IP地址。DHCP倾向于被用在需要集中管理主机的站点，而无状态自动配置不需要任何集中管理，因此后者更多地被用在典型家庭网络这样的场景下。  
使用无状态自动配置的IPv6主机可能会需要除了IP地址以外的其他信息。DHCPv6可被用来获取这样的信息，哪怕这些信息对于配置IP地址毫无用处。配置DNS服务器无需使用DHCPv6，它们可以使用无状态自动配置所需的邻居发现协议来进行配置。  
IPv6路由器，如家庭路由器，必须在无需人工干预的情况下被自动配置。这样的路由器不仅需要一个IPv6地址用来与上游路由器通信，还需要一个IPv6前缀用来配置下游的设备。DHCPv6前缀代理提供了配置此类路由器的机制。 [1]

### 实现
#### 端口号
DHCPv6客户端使用UDP端口号546，服务器使用端口号547。

#### DHCP唯一标识符
DHCP唯一标识符（DUID）用于客户端从DHCPv6服务器获得IP地址。最小长度为12个字节（96位），最大长度为20字节（160位）。实际长度取决于其类型。服务器将DUID与其数据库进行比较，并将配置数据（地址、租期、DNS服务器，等等）发送给客户端。DUID的前16位包含了DUID的三种类型之一。剩余的96位取决于DUID类型。

#### 举例
本例中，服务器的链路本地地址是fe80::0011:22ff:fe33:5566，客户端的链路本地地址是fe80::aabb:ccff:fedd:eeff。  
DHCPv6客户端从[fe80::aabb:ccff:fedd:eeff]:546 发送Solicit至 [ff02::1:2]:547。  
DHCPv6服务器从[fe80::0011:22ff:fe33:5566]:547 回应一个Advertise给 [fe80::aabb:ccff:fedd:eeff]:546。  
DHCPv6客户端从[fe80::aabb:ccff:fedd:eeff]:546 回应一个Request给 [ff02::1:2]:547。（依照RFC 3315的section 13，所有客户端消息都发送到多播地址)  
DHCPv6服务器以[fe80::0011:22ff:fe33:5566]:547 到[fe80::aabb:ccff:fedd:eeff]:546 的Reply结束。 [1]

### RA报文M/O标志位
设备在获取IPv6地址等信息时，会先发送RS报文请求链路上的路由设备，路由设备受到RS报文后会发送相应的RA报文来表示自身能够提供的IPv6服务类型。  
对于RA报文，根据其M字段和O字段确定其获取IPv6地址的模式：  
1） M字段：管理地址配置标识（Managed Address Configuration）  
M=0，标识为状态地址分配，客户端通过无状态协议（如ND）获得IPv6地址  
M=1，标识有状态地址分配，客户端通过有状态协议（如DHCPv6）获得IPv6地址  
2） O字段：其他有状态配置标识（Other Configuration）  
O=0，标识客户端通过无状态协议（如ND）获取除地址外的其他配置信息  
O=1，标识客户端通过有状态协议（如DHCPv6）获取除地址外设为其他配置信息，如DNS，SIP服务器信息。  
**协议规定，若M=1.则O=1，否则无意义。**

|M|	O|	含义|									描述|
| ----------- | ----------- |----------- |----------- |
|1|	1|	地址和DNS等都从DHCPv6服务器取得|	Stateful DHCPv6|
|0|	1|	地址使用RA广播的prefix+ EUI-64计算出来的接口地址,DNS和其他服务器从DHCPv6取得|	Stateless DHCPv6|
|0|	0|	完全的Stateless配置,仅地址使用RA广播的prefix+ EUI-64计算出来的接口地址|	Stateless AutoConfiguration|

### IETF标准
* RFC 3315, "Dynamic Host Configuration Protocol for IPv6 (DHCPv6)"
* RFC 3319, "Dynamic Host Configuration Protocol (DHCPv6) Options for Session Initiation Protocol (SIP) Servers"
* RFC 3633, "IPv6 Prefix Options for Dynamic Host Configuration Protocol (DHCP) version 6"
* RFC 3646, "DNS Configuration options for Dynamic Host Configuration Protocol for IPv6 (DHCPv6)"
* RFC 3736, "Stateless Dynamic Host Configuration Protocol (DHCP) Service for IPv6"
* RFC 5007, "DHCPv6 Leasequery"
* RFC 6221, "Lightweight DHCPv6 Relay Agent" [1] 

### IPV6地址格式
一个IPv6的地址使用冒号十六进制表示方法：128位的地址每16位分成一段，每个16位的段用十六进制表示并用冒号分隔开，例如：  
一个普通公网IPv6地址：2001:0D12:0000:0000:02AA:0987:FE29:9871  
IPv6地址支持压缩前导零的表示方法，例如上面的地址可以压缩表示为：  
2001:D12:0:0:2AA:987:FE29:9871  
为了进一步精简IPv6地址，当冒号十六进制格式中出现连续几段数值0的位段时，这些段可以压缩为双冒号的表示，例如上面的地址还可以进一步精简表示为：  
2001:D12::2AA:987:FE29:9871  
又例如IPv6的地址FF80:0:0:0:FF:3BA:891:67C2可以进一步精简表示为：  
FE80::FF:3BA:891:67C2  
这里值得注意的是，双冒号只能出现一次。  

### 设备的IPV6的地址形式
通常一个IPV6主机有多个IPV6地址，具体包括以:  
1、链路本地地址  
2、单播地址  
3、环回地址


### IPV6在Android实例

#### ifconfig的显示
```
wlan0     Link encap:Ethernet  HWaddr 52:ae:84:0d:c2:80
          inet addr:192.168.137.235  Bcast:192.168.137.255  Mask:255.255.255.0
          inet6 addr: fe80::50ae:84ff:fe0d:c280/64 Scope: Link  	
          // 本地链路地址，只用于与路由器直接通信(前缀+MAC地址生成)
          UP BROADCAST RUNNING MULTICAST  MTU:1500  Metric:1
          RX packets:1176132 errors:0 dropped:0 overruns:0 frame:0
          TX packets:1538426 errors:0 dropped:0 overruns:0 carrier:0
          collisions:0 txqueuelen:1000
          RX bytes:83630192 TX bytes:1108413359

p2p-p2p0-1 Link encap:Ethernet  HWaddr d2:80:e8:2c:8b:0c
          inet addr:192.168.49.1  Bcast:192.168.49.255  Mask:255.255.255.0
          inet6 addr: fd98:99d6:cd55:0:884b:c1e8:2321:8357/64 Scope: Global 
          // 基于地址前缀生成，后部随机，隐私IP，用于与外部通信 (前缀+随机地址生成)
          inet6 addr: fe80::d080:e8ff:fe2c:8b0c/64 Scope: Link 		    
          // 本地链路地址，只用于与路由器直接通信 (前缀+MAC地址生成)
          UP BROADCAST RUNNING MULTICAST  MTU:1500  Metric:1
          RX packets:396 errors:0 dropped:0 overruns:0 frame:0
          TX packets:170 errors:0 dropped:0 overruns:0 carrier:0
          collisions:0 txqueuelen:1000
          RX bytes:114574 TX bytes:56723

p2p-p2p0-0 Link encap:Ethernet  HWaddr 02:ef:ec:ee:66:85
          inet addr:192.168.49.1  Bcast:192.168.49.255  Mask:255.255.255.0
          inet6 addr: fe80::ef:ecff:feee:6685/64 Scope: Link 			
          // 本地链路地址，只用于与路由器直接通信  (前缀+MAC地址生成)
          inet6 addr: fd98:99d6:cd55:0:2c09:b3c9:3a3:11ad/64 Scope: Global 	
          // 基于地址前缀生成，后部随机，隐私IP，用于与外部通信 (前缀+随机地址生成)
          UP BROADCAST RUNNING MULTICAST  MTU:1500  Metric:1
          RX packets:0 errors:0 dropped:0 overruns:0 frame:0
          TX packets:19 errors:0 dropped:0 overruns:0 carrier:0
          collisions:0 txqueuelen:1000
          RX bytes:0 TX bytes:3766

p2p-p2p0-0 Link encap:Ethernet  HWaddr 62:41:ab:b3:37:f6
          inet addr:192.168.49.1  Bcast:192.168.49.255  Mask:255.255.255.0
          inet6 addr: fd98:99d6:cd55:0:ad15:d252:fcd6:1466/64 Scope: Global
          inet6 addr: fe80::6041:abff:feb3:37f6/64 Scope: Link
          UP BROADCAST RUNNING MULTICAST  MTU:1500  Metric:1
          RX packets:0 errors:0 dropped:0 overruns:0 frame:0
          TX packets:30 errors:0 dropped:0 overruns:0 carrier:0
          collisions:0 txqueuelen:1000
          RX bytes:0 TX bytes:7876

```