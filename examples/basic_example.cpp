/**
 * @file basic_example.cpp
 * @brief InduRTDB基本使用示例
 * @version 1.0.0
 * @date 2026-04-01
 * @copyright MIT License
 */

#include <indurtdb.hpp>
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    std::cout << "=== InduRTDB 基本使用示例 ===\n";
    
    try {
        // 获取InduRTDB单例
        auto& rtdb = indurtdb::InduRTDB::instance();
        
        // 初始化数据库
        std::cout << "1. 初始化数据库...\n";
        bool init_result = rtdb.initialize("example_instance", 100, 10);
        if (!init_result) {
            std::cerr << "初始化失败" << std::endl;
            return 1;
        }
        std::cout << "   ✓ 初始化成功\n";
        
        // 写入数据
        std::cout << "2. 写入数据...\n";
        rtdb.write(1001, 23.5);      // 温度
        rtdb.write(1002, 65.0);       // 湿度
        rtdb.write(2001, true);       // 水泵状态
        rtdb.write(3001, "HVAC-01");  // 设备名称
        std::cout << "   ✓ 数据写入成功\n";
        
        // 读取数据
        std::cout << "3. 读取数据...\n";
        indurtdb::PointData point;
        
        if (rtdb.read(1001, point)) {
            std::cout << "   温度: " << point.value.d << " °C\n";
            std::cout << "   质量: " << static_cast<int>(point.quality) << "\n";
            std::cout << "   时间戳: " << point.timestamp_ns << " ns\n";
        }
        
        if (rtdb.read(2001, point)) {
            std::cout << "   水泵状态: " << (point.value.b ? "运行" : "停止") << "\n";
        }
        
        // 订阅数据变化
        std::cout << "4. 订阅数据变化...\n";
        bool subscribed = rtdb.subscribe(2001, [](const indurtdb::PointData& p) {
            std::cout << "   🔔 水泵状态变化: " << (p.value.b ? "启动" : "停止") << "\n";
        });
        if (subscribed) {
            std::cout << "   ✓ 订阅成功\n";
        }
        
        // 模拟数据变化
        std::cout << "5. 模拟数据变化...\n";
        for (int i = 0; i < 3; ++i) {
            bool state = (i % 2 == 0);
            rtdb.write(2001, state);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        // 测试peek功能
        std::cout << "6. 测试peek功能...\n";
        const indurtdb::PointData* peek_data = rtdb.peek(1001);
        if (peek_data) {
            std::cout << "   温度 (peek): " << peek_data->value.d << " °C\n";
        }
        
        // 心跳更新
        std::cout << "7. 更新心跳...\n";
        rtdb.update_heartbeat();
        std::cout << "   ✓ 心跳更新成功\n";
        
        // 测试初始化状态
        std::cout << "8. 检查初始化状态...\n";
        std::cout << "   初始化状态: " << (rtdb.is_initialized() ? "已初始化" : "未初始化") << "\n";
        
        // 清理资源
        std::cout << "9. 关闭数据库...\n";
        rtdb.shutdown();
        std::cout << "   ✓ 关闭成功\n";
        
        std::cout << "\n=== 示例运行完成！ ===\n";
        
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}