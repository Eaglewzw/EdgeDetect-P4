import sys
from PyQt5.QtWidgets import (QApplication, QWidget, QVBoxLayout, QHBoxLayout,
                             QLabel, QFrame)
from PyQt5.QtCore import Qt

class DetectionUI(QWidget):
    def __init__(self):
        super().__init__()
        self.init_ui()

    def init_ui(self):
        # 1. 设置主窗口基础属性
        self.setFixedSize(800, 480)
        self.setWindowTitle('ESP32-P4 实时检测系统')
        self.setObjectName("mainWindow")

        # 2. 定义 Web 风格的全局样式 (QSS)
        self.setStyleSheet("""
            QWidget#mainWindow {
                background-color: #F8F9FA; 
                font-family: "Segoe UI", "Microsoft YaHei", sans-serif;
            }
            QLabel#title {
                font-size: 24px;
                font-weight: bold;
                color: #202124;
            }
            QLabel#device {
                font-size: 18px;
                font-weight: bold;
                color: #5F6368;
            }
            QFrame#camFrame {
                background-color: #202124; 
                border-radius: 16px;       
            }
            QLabel#camPlaceholder {
                color: #9AA0A6;
                font-size: 16px;
            }
            QFrame#infoFrame {
                background-color: #FFFFFF;
                border-radius: 16px;
                border: 1px solid #DADCE0; 
            }
            QLabel#infoTitle {
                font-size: 20px;
                font-weight: bold;
                color: #202124;
            }
            QLabel#infoText {
                font-size: 28px;
                font-weight: bold;
                color: #1A73E8; 
            }
            QLabel#labelText {
                font-size: 16px;
                color: #5F6368;
            }
            QLabel#signature {
                font-size: 15px;
                font-weight: bold;
                font-style: italic;
                color: #BDBDBD; /* 稍微调浅一点，避免抢夺主界面的注意力 */
                letter-spacing: 2px; /* 增加字母间距让大写更好看 */
            }
        """)

        # 主垂直布局
        main_layout = QVBoxLayout()
        main_layout.setContentsMargins(24, 24, 24, 16) # 左 上 右 下
        main_layout.setSpacing(16)

        # ==================== 顶部：标题栏 ====================
        header_layout = QHBoxLayout()
        
        title_label = QLabel("实时检测")
        title_label.setObjectName("title")
        
        device_label = QLabel("ESP32-P4")
        device_label.setObjectName("device")
        
        header_layout.addWidget(title_label)
        header_layout.addStretch() 
        header_layout.addWidget(device_label)
        
        main_layout.addLayout(header_layout)

        # ==================== 中部：核心内容区 ====================
        content_layout = QHBoxLayout()
        content_layout.setSpacing(24)

        # --- 左侧：摄像头显示窗口 (320x320) ---
        self.cam_frame = QFrame()
        self.cam_frame.setObjectName("camFrame")
        self.cam_frame.setFixedSize(320, 320)
        
        cam_layout = QVBoxLayout(self.cam_frame)
        self.cam_label = QLabel("等待视频流接入...") 
        self.cam_label.setObjectName("camPlaceholder")
        self.cam_label.setAlignment(Qt.AlignCenter)
        cam_layout.addWidget(self.cam_label)

        # --- 右侧：检测信息卡片 ---
        self.info_frame = QFrame()
        self.info_frame.setObjectName("infoFrame")
        
        info_layout = QVBoxLayout(self.info_frame)
        info_layout.setContentsMargins(24, 24, 24, 24)
        info_layout.setSpacing(12)

        info_title = QLabel("检测结果")
        info_title.setObjectName("infoTitle")
        
        # 类别显示
        class_hint = QLabel("识别类别 (Class)")
        class_hint.setObjectName("labelText")
        self.class_val = QLabel("Person") 
        self.class_val.setObjectName("infoText")
        
        # 概率显示
        prob_hint = QLabel("置信度 (Probability)")
        prob_hint.setObjectName("labelText")
        self.prob_val = QLabel("98.5%") 
        self.prob_val.setObjectName("infoText")
        self.prob_val.setStyleSheet("color: #34A853;") 

        info_layout.addWidget(info_title)
        info_layout.addSpacing(16)
        info_layout.addWidget(class_hint)
        info_layout.addWidget(self.class_val)
        info_layout.addSpacing(16)
        info_layout.addWidget(prob_hint)
        info_layout.addWidget(self.prob_val)
        info_layout.addStretch() 

        content_layout.addWidget(self.cam_frame)
        content_layout.addWidget(self.info_frame)
        
        main_layout.addLayout(content_layout)

        # ==================== 底部：签名 ====================
        footer_layout = QHBoxLayout()
        
        # 1. 文本改为全大写
        signature_label = QLabel("------VERSER")
        signature_label.setObjectName("signature")
        
        # 2. 先添加弹簧（Stretch），再添加组件，将文本推向右侧
        footer_layout.addStretch()
        footer_layout.addWidget(signature_label)
        
        main_layout.addLayout(footer_layout)

        # 设置主布局
        self.setLayout(main_layout)

    def update_detection(self, class_name, probability):
        self.class_val.setText(class_name)
        self.prob_val.setText(f"{probability:.1f}%")

if __name__ == '__main__':
    QApplication.setAttribute(Qt.AA_EnableHighDpiScaling)
    QApplication.setAttribute(Qt.AA_UseHighDpiPixmaps)
    
    app = QApplication(sys.argv)
    window = DetectionUI()
    window.show()
    sys.exit(app.exec_())