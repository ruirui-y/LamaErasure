#include "AutoMaskDialog.h"
#include "GlobalStruct.h"
#include <QFormLayout>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QLabel>
#include <QDoubleSpinBox>

#define MIN_R                                       10
#define MAX_R                                       200
#define MASK_NUM                                    3
#define CIRCULARITY                                 0.5
#define MARGIN                                      2

AutoMaskDialog::AutoMaskDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("AutoMask Params");
    setModal(true);

    auto* root = new QVBoxLayout(this);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);

    spMinR_ = new QSpinBox(this);
    spMinR_->setRange(1, 500);
    spMinR_->setValue(MIN_R);

    spMaxR_ = new QSpinBox(this);
    spMaxR_->setRange(1, 800);
    spMaxR_->setValue(MAX_R);

    spMaskN_ = new QSpinBox(this);
    spMaskN_->setRange(1, 20);
    spMaskN_->setValue(MASK_NUM);

    spMargin_ = new QSpinBox(this);
    spMargin_->setRange(0, 200);
    spMargin_->setValue(MARGIN);

    spCircle_ = new QDoubleSpinBox(this);
    spCircle_->setRange(0.00, 1.00);
    spCircle_->setValue(CIRCULARITY);
    
    form->addRow("Min radius (px):", spMinR_);
    form->addRow("Max radius (px):", spMaxR_);
    form->addRow("Mask count:", spMaskN_);
    form->addRow("Margin (px):", spMargin_);
    form->addRow("Circle (px):", spCircle_);

    root->addLayout(form);

    // OK/Cancel
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons_);

    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // СУ�飺min<=max
    connect(spMinR_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
        if (v > spMaxR_->value()) spMaxR_->setValue(v);
        });
    connect(spMaxR_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
        if (v < spMinR_->value()) spMinR_->setValue(v);
        });

    resize(320, 140);
}

MaskParams AutoMaskDialog::params() const
{
    MaskParams p;
    p.min_r = spMinR_->value();
    p.max_r = spMaxR_->value();
    p.mask_num = spMaskN_->value();
    p.circularity = spCircle_->value();
    p.marginPx = spMargin_->value();
    return p;
}