# dmp_msgs/RuleList - 当前生效的告警阈值集合 (Reliable+TransientLocal 锁存, 后加入的订阅者可立即拿到配置)
std_msgs/Header header
Rule[] rules
string source            # 配置来源, 例如 gateway:127.0.0.1:9100