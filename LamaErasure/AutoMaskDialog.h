#ifndef AUTO_MASK_DIALOG_H
#define AUTO_MASK_DIALOG_H

#include <QDialog>

class QSpinBox;
class QDialogButtonBox;
class QDoubleSpinBox;
struct MaskParams;

class AutoMaskDialog : public QDialog {
    Q_OBJECT
public:
    explicit AutoMaskDialog(QWidget* parent = nullptr);

    // 读取用户填写的参数
    MaskParams params() const;

private:
    QSpinBox* spMinR_ = nullptr;
    QSpinBox* spMaxR_ = nullptr;
    QSpinBox* spMaskN_ = nullptr;                                                       // 数量
    QSpinBox* spMargin_ = nullptr;                                                      // 膨胀
    QDoubleSpinBox* spCircle_ = nullptr;                                                // 圆度

    QDialogButtonBox* buttons_ = nullptr;
};

#endif // AUTO_MASK_DIALOG_H