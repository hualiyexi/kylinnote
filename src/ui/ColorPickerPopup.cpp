#include "ColorPickerPopup.h"

#include <QConicalGradient>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include "AppIcons.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {

// 自定义页内容宽度 225px（对齐滴答），卡片内边距与阴影边距。
constexpr int kContentWidth = 225;
constexpr int kCardMargin = 12;
constexpr int kShadowMargin = 12;

// 五组预设色：这是用户可选的数据，不是主题色，允许固定字面量。
struct PresetGroup {
    const char* title;
    const char* colors[8];
};
const PresetGroup kPresetGroups[] = {
    { "马卡龙", { "#F68F8A", "#F8C3B2", "#FCDEAA", "#C2DED0", "#63C5D2", "#6FAAF3", "#C09EEB", "#F678AC" } },
    { "莫兰迪", { "#D27762", "#F7C989", "#DCDE6B", "#A7C9B9", "#7BCAD3", "#86A9DE", "#AD94C7", "#C77B9B" } },
    { "洛可可", { "#BE7471", "#DD882E", "#E3CE7B", "#76A692", "#5DAF9C", "#4682B4", "#838BB0", "#CB8A90" } },
    { "经典",   { "#EC6666", "#F2B04B", "#FFD966", "#DDE358", "#5DD1A8", "#4AA6EF", "#CF66F7", "#ED70A5" } },
    { "孟菲斯", { "#EF2D24", "#FF7D5D", "#FFD457", "#02DBB2", "#34BAC5", "#8C5EFA", "#CE89FE", "#F964C2" } },
};

QColor token(ds::ColorToken t) { return ThemeManager::instance().color(t); }

QPixmap rainbowSwatchPixmap(int size)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal ring = size / 2.0 - 1.0;
    QConicalGradient gradient(size / 2.0, size / 2.0, 90);
    gradient.setColorAt(0.00, QColor(0xFF, 0x00, 0x00));
    gradient.setColorAt(0.17, QColor(0xFF, 0xD3, 0x00));
    gradient.setColorAt(0.33, QColor(0x00, 0xE5, 0x33));
    gradient.setColorAt(0.50, QColor(0x00, 0xD8, 0xFF));
    gradient.setColorAt(0.67, QColor(0x35, 0x5C, 0xFF));
    gradient.setColorAt(0.83, QColor(0xC7, 0x2A, 0xFF));
    gradient.setColorAt(1.00, QColor(0xFF, 0x00, 0x00));
    QPainterPath ringPath;
    ringPath.addEllipse(QPointF(size / 2.0, size / 2.0), ring, ring);
    ringPath.addEllipse(QPointF(size / 2.0, size / 2.0), ring * 0.62, ring * 0.62);
    painter.fillPath(ringPath, gradient);
    painter.setPen(QPen(token(ds::ColorToken::Border), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QPointF(size / 2.0, size / 2.0), ring, ring);
    return pixmap;
}

} // namespace

// ---------------------------------------------------------------------------
// 圆形预设色块：真实色值圆形；选中环与 hover 底色取主题令牌。
// ---------------------------------------------------------------------------
class PresetSwatch : public QToolButton {
public:
    explicit PresetSwatch(const QColor& color, QWidget* parent)
        : QToolButton(parent), m_color(color)
    {
        setObjectName(QStringLiteral("ColorPresetSwatch"));
        setCheckable(true);
        setFixedSize(22, 22);
        setToolTip(color.name().toUpper());
        setAccessibleName(QStringLiteral("预设颜色 %1").arg(color.name().toUpper()));
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
    }
    QColor color() const { return m_color; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (underMouse() || hasFocus()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(token(ds::ColorToken::Hover));
            painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 5, 5);
        }
        const QRectF circle(2, 2, 18, 18);
        if (isChecked()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(token(ds::ColorToken::Surface), 2.0));
            painter.drawEllipse(circle.adjusted(-1.0, -1.0, 1.0, 1.0));
            painter.setPen(QPen(token(ds::ColorToken::Primary), 2.0));
            painter.drawEllipse(circle.adjusted(-2.5, -2.5, 2.5, 2.5));
        }
        painter.setPen(QPen(token(ds::ColorToken::Border), 1.0));
        painter.setBrush(m_color);
        painter.drawEllipse(circle);
    }

private:
    QColor m_color;
};

// ---------------------------------------------------------------------------
// 饱和度/明度平面：底色为当前色相；横向叠加白→透明，纵向叠加透明→黑。
// 状态变化通过 std::function 回调上抛，避免 .cpp 内再引入 Q_OBJECT。
// ---------------------------------------------------------------------------
class SatValuePlane : public QWidget {
public:
    explicit SatValuePlane(QWidget* parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("SatValuePlane"));
        setFixedSize(kContentWidth, 144);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::CrossCursor);
        setAccessibleName(QStringLiteral("饱和度与明度"));
        setToolTip(QStringLiteral("拖动调整饱和度与明度，方向键微调，Shift+方向键精细调整"));
    }
    void setState(int hue, int sat, int val)
    {
        m_hue = hue;
        m_sat = sat;
        m_val = val;
        update();
    }
    void onChanged(const std::function<void(int, int)>& callback) { m_callback = callback; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF area = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath clip;
        clip.addRoundedRect(area, 6, 6);
        painter.setClipPath(clip);
        painter.fillRect(area, QColor::fromHsv(m_hue, 255, 255));
        QLinearGradient white(0, 0, width(), 0);
        white.setColorAt(0, QColor(255, 255, 255, 255));
        white.setColorAt(1, QColor(255, 255, 255, 0));
        painter.fillRect(area, white);
        QLinearGradient black(0, 0, 0, height());
        black.setColorAt(0, QColor(0, 0, 0, 0));
        black.setColorAt(1, QColor(0, 0, 0, 255));
        painter.fillRect(area, black);
        painter.setClipping(false);
        painter.setPen(QPen(token(ds::ColorToken::Divider), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(area, 6, 6);
        const QPointF cursor(m_sat / 255.0 * (width() - 1),
                             (255 - m_val) / 255.0 * (height() - 1));
        painter.setPen(QPen(QColor(0, 0, 0, 90), 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(cursor, 6.5, 6.5);
        painter.setPen(QPen(token(ds::ColorToken::Surface), 1.6));
        painter.drawEllipse(cursor, 6.0, 6.0);
    }
    void mousePressEvent(QMouseEvent* event) override
    {
        m_pressed = true;
        applyPosition(event->pos());
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_pressed) applyPosition(event->pos());
    }
    void mouseReleaseEvent(QMouseEvent*) override { m_pressed = false; }
    void keyPressEvent(QKeyEvent* event) override
    {
        const int step = event->modifiers() & Qt::ShiftModifier ? 1 : 5;
        int sat = m_sat, val = m_val;
        switch (event->key()) {
        case Qt::Key_Left:  sat = qMax(0, sat - step); break;
        case Qt::Key_Right: sat = qMin(255, sat + step); break;
        case Qt::Key_Up:    val = qMin(255, val + step); break;
        case Qt::Key_Down:  val = qMax(0, val - step); break;
        case Qt::Key_Home:  sat = 255; val = 255; break;
        case Qt::Key_End:   sat = 0;   val = 0;   break;
        default: QWidget::keyPressEvent(event); return;
        }
        event->accept();
        m_sat = sat;
        m_val = val;
        update();
        if (m_callback) m_callback(m_sat, m_val);
    }

private:
    void applyPosition(const QPoint& pos)
    {
        m_sat = qBound(0, qRound(pos.x() / qreal(width() - 1) * 255.0), 255);
        m_val = 255 - qBound(0, qRound(pos.y() / qreal(height() - 1) * 255.0), 255);
        update();
        if (m_callback) m_callback(m_sat, m_val);
    }
    int m_hue = 0, m_sat = 255, m_val = 255;
    bool m_pressed = false;
    std::function<void(int, int)> m_callback;
};

// ---------------------------------------------------------------------------
// 横向 hue 彩虹条。
// ---------------------------------------------------------------------------
class HueBar : public QWidget {
public:
    explicit HueBar(QWidget* parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("HueBar"));
        setFixedSize(kContentWidth, 14);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setAccessibleName(QStringLiteral("色相"));
        setToolTip(QStringLiteral("拖动调整色相，左右方向键微调"));
    }
    void setHue(int hue) { m_hue = hue; update(); }
    void onChanged(const std::function<void(int)>& callback) { m_callback = callback; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF area = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath clip;
        clip.addRoundedRect(area, 7, 7);
        painter.setClipPath(clip);
        QLinearGradient gradient(0, 0, width(), 0);
        for (int i = 0; i <= 6; ++i)
            gradient.setColorAt(i / 6.0, QColor::fromHsv((i * 60) % 360, 255, 255));
        painter.fillRect(area, gradient);
        painter.setClipping(false);
        painter.setPen(QPen(token(ds::ColorToken::Divider), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(area, 7, 7);
        const qreal x = m_hue / 359.0 * (width() - 1);
        painter.setPen(QPen(QColor(0, 0, 0, 90), 2.0));
        painter.drawLine(QPointF(x, 0.5), QPointF(x, height() - 0.5));
        painter.setPen(QPen(token(ds::ColorToken::Surface), 1.4));
        painter.drawLine(QPointF(x, 1.0), QPointF(x, height() - 1.0));
    }
    void mousePressEvent(QMouseEvent* event) override
    {
        m_pressed = true;
        applyPosition(event->pos());
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_pressed) applyPosition(event->pos());
    }
    void mouseReleaseEvent(QMouseEvent*) override { m_pressed = false; }
    void keyPressEvent(QKeyEvent* event) override
    {
        const int step = event->modifiers() & Qt::ShiftModifier ? 1 : 5;
        int hue = m_hue;
        switch (event->key()) {
        case Qt::Key_Left:  hue = (hue + 360 - step) % 360; break;
        case Qt::Key_Right: hue = (hue + step) % 360; break;
        default: QWidget::keyPressEvent(event); return;
        }
        event->accept();
        m_hue = hue;
        update();
        if (m_callback) m_callback(m_hue);
    }

private:
    void applyPosition(const QPoint& pos)
    {
        m_hue = qBound(0, qRound(pos.x() / qreal(width() - 1) * 359.0), 359);
        update();
        if (m_callback) m_callback(m_hue);
    }
    int m_hue = 0;
    bool m_pressed = false;
    std::function<void(int)> m_callback;
};

// ---------------------------------------------------------------------------
// 圆形颜色预览。
// ---------------------------------------------------------------------------
class ColorPreviewDot : public QWidget {
public:
    explicit ColorPreviewDot(QWidget* parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("ColorPreview"));
        setFixedSize(28, 28);
        setAccessibleName(QStringLiteral("当前颜色预览"));
    }
    void setColor(const QColor& color) { m_color = color; update(); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF circle(2, 2, 24, 24);
        painter.setPen(QPen(token(ds::ColorToken::Border), 1.0));
        painter.setBrush(m_color);
        painter.drawEllipse(circle);
    }

private:
    QColor m_color = Qt::red;
};

// ---------------------------------------------------------------------------
// ColorPickerPopup
// ---------------------------------------------------------------------------
ColorPickerPopup::ColorPickerPopup(QWidget* parent)
    : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setObjectName(QStringLiteral("ColorPickerPopup"));
    setWindowTitle(QStringLiteral("选择颜色"));
    setAttribute(Qt::WA_TranslucentBackground);
    setFont(ds::qfont(ds::FontScene::Base));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kShadowMargin, kShadowMargin, kShadowMargin, kShadowMargin);
    outer->setSpacing(0);

    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("ColorPickerCard"));
    m_card->setAttribute(Qt::WA_StyledBackground);
    m_card->setFixedWidth(kContentWidth + 2 * kCardMargin);
    auto* shadow = new QGraphicsDropShadowEffect(m_card);
    shadow->setBlurRadius(24);
    shadow->setOffset(0, 4);
    m_card->setGraphicsEffect(shadow);
    outer->addWidget(m_card, 0, Qt::AlignCenter);

    auto* cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);
    m_pages = new QStackedWidget(m_card);
    m_pages->setObjectName(QStringLiteral("ColorPickerPages"));
    cardLayout->addWidget(m_pages);

    buildPresetPage();
    buildCustomPage();
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](ds::Theme) { applyTheme(); });
}

ColorPickerPopup::~ColorPickerPopup() = default;

void ColorPickerPopup::buildPresetPage()
{
    auto* page = new QWidget(m_pages);
    page->setObjectName(QStringLiteral("ColorPresetPage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(kCardMargin, 10, kCardMargin, kCardMargin);
    layout->setSpacing(5);

    for (const PresetGroup& group : kPresetGroups) {
        auto* title = new QLabel(QString::fromUtf8(group.title), page);
        title->setProperty("muted", true);
        title->setFont(ds::qfont(ds::FontScene::Caption));
        layout->addWidget(title);
        auto* row = new QHBoxLayout();
        row->setSpacing(7);
        for (const char* hex : group.colors) {
            const QColor color(QString::fromLatin1(hex));
            auto* swatch = new PresetSwatch(color, page);
            swatch->setProperty("hex", color.name());
            connect(swatch, &QToolButton::clicked, this,
                    [this, swatch]() { selectColor(swatch->color()); });
            m_presetSwatches.append(swatch);
            row->addWidget(swatch);
        }
        row->addStretch(1);
        layout->addLayout(row);
    }

    m_customEntry = new QPushButton(page);
    m_customEntry->setObjectName(QStringLiteral("ColorCustomEntry"));
    m_customEntry->setText(QStringLiteral("自定义颜色"));
    m_customEntry->setIconSize(QSize(18, 18));
    m_customEntry->setIcon(QIcon(rainbowSwatchPixmap(18)));
    m_customEntry->setFixedHeight(36);
    m_customEntry->setToolTip(QStringLiteral("打开自定义颜色选择器"));
    m_customEntry->setAccessibleName(QStringLiteral("自定义颜色"));
    m_customEntry->setAutoDefault(false);
    connect(m_customEntry, &QPushButton::clicked, this, &ColorPickerPopup::showCustomPage);
    layout->addSpacing(4);
    layout->addWidget(m_customEntry);

    m_pages->addWidget(page);
}

void ColorPickerPopup::buildCustomPage()
{
    auto* page = new QWidget(m_pages);
    page->setObjectName(QStringLiteral("ColorCustomPage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(kCardMargin, 8, kCardMargin, kCardMargin);
    layout->setSpacing(8);

    auto* header = new QHBoxLayout();
    header->setSpacing(6);
    m_backButton = new QToolButton(page);
    m_backButton->setObjectName(QStringLiteral("ColorCustomBack"));
    m_backButton->setFixedSize(26, 26);
    m_backButton->setToolTip(QStringLiteral("返回预设颜色"));
    m_backButton->setAccessibleName(QStringLiteral("返回预设颜色"));
    connect(m_backButton, &QToolButton::clicked, this, &ColorPickerPopup::showPresetPage);
    auto* title = new QLabel(QStringLiteral("自定义颜色"), page);
    title->setFont(ds::qfont(ds::FontScene::Section));
    header->addWidget(m_backButton);
    header->addWidget(title);
    header->addStretch(1);
    layout->addLayout(header);

    m_plane = new SatValuePlane(page);
    m_plane->onChanged([this](int sat, int val) {
        m_customDirty = true;
        m_sat = sat;
        m_val = val;
        refreshCustomWidgets();
    });
    layout->addWidget(m_plane);

    m_hueBar = new HueBar(page);
    m_hueBar->onChanged([this](int hue) {
        m_customDirty = true;
        m_hue = hue;
        refreshCustomWidgets();
    });
    layout->addWidget(m_hueBar);

    auto* hexRow = new QHBoxLayout();
    hexRow->setSpacing(6);
    m_previewDot = new ColorPreviewDot(page);
    hexRow->addWidget(m_previewDot);
    auto* hash = new QLabel(QStringLiteral("#"), page);
    hexRow->addWidget(hash);
    m_hexEdit = new QLineEdit(page);
    m_hexEdit->setObjectName(QStringLiteral("ColorHexEdit"));
    m_hexEdit->setPlaceholderText(QStringLiteral("RRGGBB"));
    m_hexEdit->setMaxLength(6);
    m_hexEdit->setAccessibleName(QStringLiteral("十六进制颜色值"));
    m_hexEdit->setFont(ds::qfont(ds::FontScene::Mono));
    connect(m_hexEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        // 回填自身的程序化 setText 不算用户编辑（否则 HSV 回写会误标记 dirty）。
        if (m_updatingHex) return;
        static const QRegularExpression format(QStringLiteral("^[0-9A-Fa-f]{6}$"));
        if (format.match(text).hasMatch()) {
            m_customDirty = true;
            m_updatingHex = true;
            syncCustomFromColor(QColor(QStringLiteral("#") + text.toUpper()));
            m_updatingHex = false;
        }
    });
    connect(m_hexEdit, &QLineEdit::returnPressed, this, [this]() { commitCustomColor(); });
    hexRow->addWidget(m_hexEdit, 1);
    auto* hexTag = new QLabel(QStringLiteral("hex"), page);
    hexTag->setObjectName(QStringLiteral("ColorHexTag"));
    hexTag->setProperty("muted", true);
    hexTag->setFont(ds::qfont(ds::FontScene::Caption));
    hexRow->addWidget(hexTag);
    layout->addLayout(hexRow);

    m_hexError = new QLabel(page);
    m_hexError->setObjectName(QStringLiteral("ColorHexError"));
    m_hexError->hide();
    layout->addWidget(m_hexError);

    auto* actions = new QHBoxLayout();
    actions->setSpacing(8);
    auto* cancelButton = new QPushButton(QStringLiteral("取消"), page);
    cancelButton->setObjectName(QStringLiteral("ColorCancel"));
    cancelButton->setAutoDefault(false);
    cancelButton->setFixedHeight(32);
    connect(cancelButton, &QPushButton::clicked, this, &ColorPickerPopup::showPresetPage);
    auto* saveButton = new QPushButton(QStringLiteral("保存"), page);
    saveButton->setObjectName(QStringLiteral("ColorSave"));
    saveButton->setProperty("primary", true);
    saveButton->setAutoDefault(false);
    saveButton->setFixedHeight(32);
    connect(saveButton, &QPushButton::clicked, this, [this]() { commitCustomColor(); });
    actions->addWidget(cancelButton, 1);
    actions->addWidget(saveButton, 1);
    layout->addLayout(actions);

    m_pages->addWidget(page);
}

void ColorPickerPopup::applyTheme()
{
    m_card->setStyleSheet(QStringLiteral(
        "QWidget#ColorPickerCard{background:%1;border:1px solid %2;border-radius:12px;}"
        "QLabel{background:transparent;border:0;color:%3;}"
        "QLabel[muted=\"true\"]{color:%4;}"
        "QLabel#ColorHexError{color:%5;}"
        "QPushButton#ColorCustomEntry{color:%3;background:transparent;"
        "border:1px solid transparent;border-radius:8px;padding:0 8px;text-align:left;}"
        "QPushButton#ColorCustomEntry:hover{background:%6;}"
        "QPushButton#ColorCustomEntry:focus{background:%6;}"
        "QPushButton#ColorCancel{color:%4;background:%8;border:1px solid %2;"
        "border-radius:6px;}"
        "QPushButton#ColorCancel:hover{background:%9;}"
        "QPushButton#ColorCancel:focus{background:%9;}"
        "QPushButton#ColorSave{color:%10;background:%11;border:1px solid %11;"
        "border-radius:6px;}"
        "QPushButton#ColorSave:hover{background:%12;border-color:%12;}"
        "QPushButton#ColorSave:focus{border-color:%11;}"
        "QLineEdit#ColorHexEdit{color:%3;background:%8;border:1px solid %2;"
        "border-radius:6px;padding:5px 8px;selection-background-color:%11;"
        "selection-color:%10;}"
        "QLineEdit#ColorHexEdit:focus{background:%9;}"
        "QToolButton#ColorCustomBack{background:transparent;"
        "border:1px solid transparent;border-radius:6px;padding:0;}"
        "QToolButton#ColorCustomBack:hover{background:%6;}"
        "QToolButton#ColorCustomBack:focus{background:%6;}")
        .arg(token(ds::ColorToken::MenuBg).name())
        .arg(token(ds::ColorToken::Divider).name())
        .arg(token(ds::ColorToken::Text).name())
        .arg(token(ds::ColorToken::TextSecondary).name())
        .arg(token(ds::ColorToken::Danger).name())
        .arg(token(ds::ColorToken::Hover).name())
        .arg(token(ds::ColorToken::FocusRing).name())
        .arg(token(ds::ColorToken::Field).name())
        .arg(token(ds::ColorToken::FieldHover).name())
        .arg(token(ds::ColorToken::OnPrimary).name())
        .arg(token(ds::ColorToken::Primary).name())
        .arg(token(ds::ColorToken::PrimaryHover).name()));
    if (auto* effect = qobject_cast<QGraphicsDropShadowEffect*>(m_card->graphicsEffect())) {
        QColor tint = token(ds::ColorToken::Shadow);
        tint.setAlpha(40);
        effect->setColor(tint);
    }
    m_backButton->setIcon(AppIcons::icon(QStringLiteral("chevron-left"),
                                         ds::ColorToken::TextSecondary, 16));
    QPalette palette = m_hexEdit->palette();
    palette.setColor(QPalette::PlaceholderText, token(ds::ColorToken::TextTertiary));
    m_hexEdit->setPalette(palette);
    // 彩虹环带主题边框色，主题切换后重建。
    m_customEntry->setIcon(QIcon(rainbowSwatchPixmap(18)));
    m_card->update();
}

void ColorPickerPopup::setInitialColor(const QColor& color)
{
    m_initial = color;
    m_customDirty = false;
    if (color.isValid()) syncCustomFromColor(color);
    // 预设页选中态：与初始色相同的色块打勾（按 hex 属性比较，忽略大小写）。
    for (QToolButton* button : m_presetSwatches) {
        button->setChecked(color.isValid()
                           && button->property("hex").toString()
                                  .compare(color.name(), Qt::CaseInsensitive) == 0);
    }
}

QColor ColorPickerPopup::currentColor() const
{
    // 未改动时回传精确初始色：QColor 的 RGB↔HSV 往返存在舍入，避免原色漂移。
    if (!m_customDirty && m_initial.isValid()) return m_initial;
    return QColor::fromHsv(m_hue, m_sat, m_val);
}

void ColorPickerPopup::syncCustomFromColor(const QColor& color)
{
    if (!color.isValid()) return;
    m_hue = color.hue();
    m_sat = color.saturation();
    m_val = color.value();
    if (m_hue < 0) m_hue = 0; // 灰色系 hue() 为 -1，保持 0（红），不影响结果
    refreshCustomWidgets();
}

void ColorPickerPopup::refreshCustomWidgets()
{
    const QColor color = currentColor();
    m_plane->setState(m_hue, m_sat, m_val);
    m_hueBar->setHue(m_hue);
    m_previewDot->setColor(color);
    if (!m_updatingHex) {
        // 程序化回写 HEX 时置标志，textChanged 回调据此跳过，避免误标 dirty 与递归。
        m_updatingHex = true;
        m_hexEdit->setText(color.name().mid(1).toUpper()); // 去掉 #，仅 RRGGBB
        m_updatingHex = false;
        m_hexError->hide();
    }
}

bool ColorPickerPopup::commitCustomColor()
{
    static const QRegularExpression format(QStringLiteral("^[0-9A-Fa-f]{6}$"));
    const QString text = m_hexEdit->text().trimmed();
    if (!format.match(text).hasMatch()) {
        m_hexError->setText(QStringLiteral("请输入 6 位十六进制颜色值，如 66CCFF"));
        m_hexError->show();
        m_hexEdit->setFocus();
        m_hexEdit->selectAll();
        return false;
    }
    selectColor(QColor(QStringLiteral("#") + text.toUpper()));
    return true;
}

void ColorPickerPopup::selectColor(const QColor& color)
{
    // 统一转成 Rgb spec 再外发：Qt 5.12 中 Hsv spec 与 Rgb spec 的 QColor
    // 即使 RGB 相同，operator== 也判不相等，接收方直接比较会踩坑。
    const QColor normalized(color.rgb());
    hide();
    emit colorSelected(normalized);
    accept();
}

void ColorPickerPopup::showPresetPage()
{
    m_pages->setCurrentIndex(0);
}

void ColorPickerPopup::showCustomPage()
{
    m_pages->setCurrentIndex(1);
    m_plane->setFocus();
}

void ColorPickerPopup::popupAt(QWidget* anchor, bool above)
{
    if (!anchor) return;
    layout()->activate();
    adjustSize();
    const QPoint top = anchor->mapToGlobal(QPoint(0, 0));
    const QPoint bottom = anchor->mapToGlobal(QPoint(0, anchor->height()));
    QPoint position(top.x() - kShadowMargin,
                    above ? top.y() - height() + kShadowMargin - 4
                          : bottom.y() - kShadowMargin + 4);
    QScreen* screen = QGuiApplication::screenAt(top);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect available = screen->availableGeometry();
        if (above && position.y() < available.top()) position.setY(bottom.y() + 4);
        if (!above && position.y() + height() > available.bottom())
            position.setY(top.y() - height() - 4);
        position.setX(qBound(available.left(), position.x(),
                             qMax(available.left(), available.right() - width() + 1)));
        position.setY(qBound(available.top(), position.y(),
                             qMax(available.top(), available.bottom() - height() + 1)));
    }
    move(position);
    show();
    raise();
}

} // namespace kl
