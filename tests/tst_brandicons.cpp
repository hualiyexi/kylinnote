#include <QtTest/QtTest>

#include <cstdio>
#include <memory>

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QFrame>
#include <QLayout>
#include <QToolButton>
#include <QPixmap>
#include <QSettings>
#include <QTemporaryDir>

#include "Common.h"
#include "app/Branding.h"
#include "app/TrayController.h"
#include "app/TaskStore.h"
#include "db/Database.h"
#include "ui/AppStyle.h"
#include "ui/TaskPanel.h"
#include "ui/BrandingIcons.h"
#include "ui/WorkspaceGeometry.h"
#include "ui/design/DesignTokens.h"
#include "ui/design/ThemeManager.h"

// 品牌图标 UI 层单元测试：只使用 QTemporaryDir 与合成 PNG 素材（非方形、
// 带透明边角、纯色块），通过 KYLINLIST_BRANDING_DIR / KYLINLIST_DATA_DIR
// 隔离，不接触真实品牌配置与真实 Logo，不访问网络，不写用户设置。
//
// 覆盖：
// - brandLogoIcon()：配置命中时原样透传（原生尺寸/透明度不被改写），
//   无配置/坏图回退默认品牌标（非空）；
// - applicationBrandIcon()：同一 Logo 的白底软件版，固定白画布来自
//   AppIconBg 令牌，等比居中不裁剪不染色，原图透明处变白；
// - 多尺寸 + 2x DPR、配置显式重载后重建、产品身份不变、源文件不被改写；
// - TrayController::trayIcon() 委托 applicationBrandIcon()。
//
// 自定义 main：AA_UseHighDpiPixmaps 与 src/main.cpp 保持一致，才能覆盖
// 高 DPR 图标路径；offscreen 兜底必须在 QApplication 构造前设置。

using namespace kl;

namespace {

const QString kManifestName = QStringLiteral("branding.json");
// 合成 Logo：48×24（2:1 非方形），完全透明底 + 居中 24×12 不透明色块，
// 四个边角与色块四周均为透明。
const int kLogoWidth = 48;
const int kLogoHeight = 24;
const QRect kBlockRect(12, 6, 24, 12);
const QRgb kRedBlock = qRgb(0xD9, 0x2B, 0x2B);
const QRgb kBlueBlock = qRgb(0x2B, 0x57, 0xD9);
// 默认品牌标的主色方块颜色（即 ds::raw::Primary）。
const QRgb kPrimary = ds::raw::Primary;

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(bytes);
    file.close();
    return file.error() == QFile::NoError;
}

bool writeManifest(const QString &dir, const QString &json)
{
    if (!QDir().mkpath(dir))
        return false;
    return writeFile(QDir(dir).filePath(kManifestName), json.toUtf8());
}

// 写入合成非方形透明 Logo；blockColor 决定色块颜色（红/蓝两版，几何相同）。
bool writeSyntheticLogo(const QString &path, QRgb blockColor)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QImage image(kLogoWidth, kLogoHeight, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.fillRect(kBlockRect, QColor(blockColor));
    }
    return image.save(path, "png");
}

QByteArray readFileBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

QImage iconImage(const QIcon &icon, const QSize &size)
{
    return icon.pixmap(size).toImage();
}

} // namespace

class BrandIconsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void tokenAppIconBgFixedWhite();
    void noConfigUsesDefaultMark();
    void customLogoPassedThroughUnmodified();
    void appVersionWhiteCanvasAndAspect();
    void appVersionMultiSizeAndDpr();
    void trayIconDelegatesToAppVersion();
    void reloadRebuildsIcons();
    void brokenLogoFallsBackToDefault();
    void identityStableUnderBranding();
    void sourceFileNeverRewritten();
    void firstRailRowUsesTransparentBrandLogo();

private:
    QString envBrandingDir() const { return m_brandingDir->path(); }
    QString logoPath() const
    { return QDir(envBrandingDir()).filePath(QStringLiteral("mark.png")); }

    std::unique_ptr<QTemporaryDir> m_settingsDir;
    std::unique_ptr<QTemporaryDir> m_dataDir;
    std::unique_ptr<QTemporaryDir> m_brandingDir;
    QByteArray m_originalDataDir, m_originalBrandingDir;
    bool m_hadDataDir = false, m_hadBrandingDir = false;
    QSettings::Format m_settingsFormat = QSettings::NativeFormat;
};

void BrandIconsTest::initTestCase()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    m_hadDataDir = qEnvironmentVariableIsSet("KYLINLIST_DATA_DIR");
    m_hadBrandingDir = qEnvironmentVariableIsSet("KYLINLIST_BRANDING_DIR");
    m_originalDataDir = qgetenv("KYLINLIST_DATA_DIR");
    m_originalBrandingDir = qgetenv("KYLINLIST_BRANDING_DIR");
    m_settingsFormat = QSettings::defaultFormat();
    m_settingsDir.reset(new QTemporaryDir);
    QVERIFY(m_settingsDir->isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir->path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, m_settingsDir->path() + QStringLiteral("/system"));
}

void BrandIconsTest::cleanupTestCase()
{
    QSettings::setDefaultFormat(m_settingsFormat);
    if (m_hadDataDir) qputenv("KYLINLIST_DATA_DIR", m_originalDataDir);
    else qunsetenv("KYLINLIST_DATA_DIR");
    if (m_hadBrandingDir) qputenv("KYLINLIST_BRANDING_DIR", m_originalBrandingDir);
    else qunsetenv("KYLINLIST_BRANDING_DIR");
}

void BrandIconsTest::init()
{
    m_dataDir.reset(new QTemporaryDir);
    m_brandingDir.reset(new QTemporaryDir);
    QVERIFY2(m_dataDir->isValid(), "临时数据目录创建失败");
    QVERIFY2(m_brandingDir->isValid(), "临时品牌目录创建失败");
    qputenv("KYLINLIST_DATA_DIR", m_dataDir->path().toUtf8());
    qputenv("KYLINLIST_BRANDING_DIR", m_brandingDir->path().toUtf8());
    Branding::load();
    ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
}

void BrandIconsTest::cleanup()
{
    Database::instance().close();
    qunsetenv("KYLINLIST_DATA_DIR");
    qunsetenv("KYLINLIST_BRANDING_DIR");
    m_dataDir.reset();
    m_brandingDir.reset();
}

// 软件图标底色不随主题变化。
void BrandIconsTest::tokenAppIconBgFixedWhite()
{
    QCOMPARE(ds::color(ds::ColorToken::AppIconBg, ds::Theme::Light),
             QColor(ds::raw::White));
    QCOMPARE(ds::color(ds::ColorToken::AppIconBg, ds::Theme::Dark),
             QColor(ds::raw::White));
    const size_t idx = static_cast<size_t>(ds::ColorToken::AppIconBg);
    QCOMPARE(ds::colorTable(ds::Theme::Light)[idx], ds::raw::White);
    QCOMPARE(ds::colorTable(ds::Theme::Dark)[idx], ds::raw::White);
    QVERIFY(writeSyntheticLogo(logoPath(), kRedBlock));
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"logo\":\"mark.png\"}")));
    Branding::load();
    for (ThemeManager::Preference theme : {ThemeManager::Preference::Light, ThemeManager::Preference::Dark}) {
        ThemeManager::instance().setPreference(theme);
        const QImage image = iconImage(applicationBrandIcon(), QSize(64, 64));
        QCOMPARE(image.pixel(0, 0), ds::raw::White);
        QCOMPARE(image.pixel(32, 32), kRedBlock);
    }
}

// 无品牌配置：两个入口都回退默认品牌标（非空），core 的只读语义不变。
void BrandIconsTest::noConfigUsesDefaultMark()
{
    Branding::load();
    QVERIFY(Branding::appIcon().isNull());  // core 提供者语义保持：未配置为空

    const QIcon logo = brandLogoIcon();
    QVERIFY2(!logo.isNull(), "无配置时 brandLogoIcon 应回退默认品牌标而非空图标");
    const QImage logoImg = iconImage(logo, QSize(64, 64));
    QVERIFY2(logoImg.size() == QSize(64, 64), "默认标应为方形画布");
    QCOMPARE(qAlpha(logoImg.pixel(2, 2)), 0);          // 圆角外保持透明
    QCOMPARE(logoImg.pixel(57, 32), kPrimary);         // 主色方块（对勾区右侧）

    const QIcon app = applicationBrandIcon();
    QVERIFY(!app.isNull());
    const QImage appImg = iconImage(app, QSize(64, 64));
    QCOMPARE(appImg.pixel(2, 2), QRgb(0xFFFFFFFF));    // 原透明角落 → 固定白
    QCOMPARE(appImg.pixel(57, 32), kPrimary);          // 默认标原样叠在白底上
}

// 配置命中：原始 Logo 原样透传，原生尺寸（非方形）与透明度不被改写。
void BrandIconsTest::customLogoPassedThroughUnmodified()
{
    QVERIFY(writeSyntheticLogo(logoPath(), kRedBlock));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"甲品牌\",\"logo\":\"mark.png\"}")));
    Branding::load();
    QVERIFY(!Branding::appIcon().isNull());

    const QIcon logo = brandLogoIcon();
    QVERIFY(!logo.isNull());
    // 先按原生尺寸取一次像素，再断言固有尺寸保持 48×24（非方形不被动过）。
    const QImage native = iconImage(logo, QSize(kLogoWidth, kLogoHeight));
    QVERIFY2(native.size() == QSize(kLogoWidth, kLogoHeight), "原生尺寸请求应返回原生像素");
    QCOMPARE(logo.actualSize(QSize(4096, 4096)), QSize(kLogoWidth, kLogoHeight));
    QCOMPARE(qAlpha(native.pixel(0, 0)), 0);                    // 边角透明
    QCOMPARE(qAlpha(native.pixel(24, 3)), 0);                   // 色块上方透明
    QCOMPARE(qAlpha(native.pixel(4, 12)), 0);                   // 色块左侧透明
    QCOMPARE(native.pixel(24, 12), kRedBlock);                  // 色块颜色原样，未染色
}

// 白底软件版：固定白画布、等比居中（48×24 → 64×32，上下各留 16px 白边），
// 色块颜色不改变；原图透明处（含四角与画边距）一律变不透明白。
void BrandIconsTest::appVersionWhiteCanvasAndAspect()
{
    QVERIFY(writeSyntheticLogo(logoPath(), kRedBlock));
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"logo\":\"mark.png\"}")));
    Branding::load();

    const QImage img = iconImage(applicationBrandIcon(), QSize(64, 64));
    QVERIFY2(img.size() == QSize(64, 64), "软件版应为方形画布");
    QCOMPARE(img.pixel(0, 0), QRgb(0xFFFFFFFF));    // 原透明边角 → 白
    QCOMPARE(img.pixel(32, 8), QRgb(0xFFFFFFFF));   // 等比留边（logo 顶边之上）→ 白
    QCOMPARE(img.pixel(4, 32), QRgb(0xFFFFFFFF));   // 原图透明左边距 → 白
    QCOMPARE(img.pixel(32, 20), QRgb(0xFFFFFFFF));  // logo 内部的透明区 → 白
    QCOMPARE(img.pixel(32, 32), kRedBlock);         // 色块中心：不裁剪、不染色
}

// 软件版多尺寸与高 DPR：尺寸阶梯齐全，2x 请求返回 DPR=2 的像素图。
void BrandIconsTest::appVersionMultiSizeAndDpr()
{
    QVERIFY(writeSyntheticLogo(logoPath(), kRedBlock));
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"logo\":\"mark.png\"}")));
    Branding::load();

    const QIcon app = applicationBrandIcon();
    QVERIFY(app.availableSizes().count() >= 7);
    const int ladder[] = {16, 24, 32, 48, 64, 128, 256};
    for (int s : ladder) {
        const QPixmap pm = app.pixmap(QSize(s, s));
        QVERIFY2(!pm.isNull(), qPrintable(QStringLiteral("尺寸 %1 应可用").arg(s)));
        QCOMPARE(qRound(pm.width() / pm.devicePixelRatio()), s);
        QCOMPARE(qRound(pm.height() / pm.devicePixelRatio()), s);
    }
    QImage hi(64, 64, QImage::Format_ARGB32_Premultiplied);
    hi.setDevicePixelRatio(2.0);
    hi.fill(Qt::transparent);
    {
        QPainter painter(&hi);
        app.paint(&painter, QRect(0, 0, 32, 32));
    }
    QCOMPARE(hi.devicePixelRatio(), 2.0);
    QCOMPARE(hi.pixel(0, 0), ds::raw::White);
    QCOMPARE(hi.pixel(32, 32), kRedBlock);
    QCOMPARE(hi.pixel(32, 8), ds::raw::White);
}

// TrayController::trayIcon() 委托软件版：无配置与有配置两种情况下逐像素一致。
void BrandIconsTest::trayIconDelegatesToAppVersion()
{
    Branding::load();  // 无配置
    {
        const QImage tray = TrayController::trayIcon().pixmap(QSize(64, 64)).toImage();
        const QImage app = applicationBrandIcon().pixmap(QSize(64, 64)).toImage();
        QVERIFY2(tray == app, "无配置时 trayIcon() 应与 applicationBrandIcon() 输出一致");
    }

    QVERIFY(writeSyntheticLogo(logoPath(), kBlueBlock));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"乙品牌\",\"logo\":\"mark.png\"}")));
    Branding::load();
    const QImage tray = TrayController::trayIcon().pixmap(QSize(64, 64)).toImage();
    const QImage app = applicationBrandIcon().pixmap(QSize(64, 64)).toImage();
    QVERIFY2(tray == app, "有配置时 trayIcon() 应与 applicationBrandIcon() 输出一致");
    QCOMPARE(tray.pixel(32, 32), kBlueBlock);
    QCOMPARE(tray.pixel(0, 0), QRgb(0xFFFFFFFF));
}

// 显式 Branding::load() 后必须重建：同一路径、同样尺寸的素材原地换内容，
// 路径/大小都可能不变，旧渲染不得复用。
void BrandIconsTest::reloadRebuildsIcons()
{
    QVERIFY(writeSyntheticLogo(logoPath(), kRedBlock));
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"logo\":\"mark.png\"}")));
    Branding::load();
    QCOMPARE(iconImage(brandLogoIcon(), QSize(kLogoWidth, kLogoHeight)).pixel(24, 12), kRedBlock);
    QCOMPARE(iconImage(applicationBrandIcon(), QSize(64, 64)).pixel(32, 32), kRedBlock);

    // 同路径同尺寸，仅内容红→蓝；显式重读后两个入口都应换成蓝色。
    QVERIFY(writeSyntheticLogo(logoPath(), kBlueBlock));
    Branding::load();
    QCOMPARE(iconImage(brandLogoIcon(), QSize(kLogoWidth, kLogoHeight)).pixel(24, 12), kBlueBlock);
    QCOMPARE(iconImage(applicationBrandIcon(), QSize(64, 64)).pixel(32, 32), kBlueBlock);

    // 配置改为仅名称：回退默认品牌标。
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"displayName\":\"仅名称\"}")));
    Branding::load();
    const QImage logo = iconImage(brandLogoIcon(), QSize(64, 64));
    QCOMPARE(qAlpha(logo.pixel(2, 2)), 0);
    QCOMPARE(logo.pixel(57, 32), kPrimary);
    QCOMPARE(iconImage(applicationBrandIcon(), QSize(64, 64)).pixel(2, 2), QRgb(0xFFFFFFFF));
}

// 坏图只回退图标，不影响显示名；core 的 appIcon() 语义仍是空。
void BrandIconsTest::brokenLogoFallsBackToDefault()
{
    QVERIFY(writeFile(logoPath(), QByteArrayLiteral("这只是文本，不是PNG数据")));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"名称不受影响\",\"logo\":\"mark.png\"}")));
    Branding::load();

    QCOMPARE(Branding::displayName(), QStringLiteral("名称不受影响"));
    QVERIFY(Branding::appIcon().isNull());

    const QIcon logoIcon = brandLogoIcon();
    QVERIFY(!logoIcon.isNull());
    const QImage logo = iconImage(logoIcon, QSize(64, 64));
    QCOMPARE(qAlpha(logo.pixel(2, 2)), 0);
    QCOMPARE(logo.pixel(57, 32), kPrimary);
    const QImage app = iconImage(applicationBrandIcon(), QSize(64, 64));
    QCOMPARE(app.pixel(2, 2), QRgb(0xFFFFFFFF));
    QCOMPARE(app.pixel(57, 32), kPrimary);
}

// 品牌定制只影响显示层：内部产品身份与数据目录不随品牌图标变化。
void BrandIconsTest::identityStableUnderBranding()
{
    QVERIFY(writeSyntheticLogo(logoPath(), kRedBlock));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"完全不同的品牌\",\"logo\":\"mark.png\"}")));
    Branding::load();
    brandLogoIcon();
    applicationBrandIcon();
    TrayController::trayIcon();

    QCOMPARE(productId(), QStringLiteral("kylinlist"));
    QCOMPARE(productName(), QStringLiteral("KylinList"));
    QCOMPARE(productEnvironmentPrefix(), QStringLiteral("KYLINLIST"));
    QCOMPARE(dataDirPath(), QDir(m_dataDir->path()).absolutePath());
    QCOMPARE(databasePath(), QDir(m_dataDir->path()).absolutePath() + QStringLiteral("/kylinlist.db"));
    QCOMPARE(Branding::displayName(), QStringLiteral("完全不同的品牌"));
}

// 图标构建只读：源 PNG 与 manifest 在取图前后字节完全一致。
void BrandIconsTest::sourceFileNeverRewritten()
{
    QVERIFY(writeSyntheticLogo(logoPath(), kRedBlock));
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"logo\":\"mark.png\"}")));
    Branding::load();
    const QByteArray logoBefore = readFileBytes(logoPath());
    const QByteArray manifestBefore = readFileBytes(
        QDir(envBrandingDir()).filePath(kManifestName));
    QVERIFY(!logoBefore.isEmpty());
    QVERIFY(!manifestBefore.isEmpty());

    brandLogoIcon();
    applicationBrandIcon();
    TrayController::trayIcon();
    brandLogoIcon().pixmap(QSize(kLogoWidth, kLogoHeight)).toImage();
    applicationBrandIcon().pixmap(QSize(256, 256)).toImage();

    QCOMPARE(readFileBytes(logoPath()), logoBefore);
    QCOMPARE(readFileBytes(QDir(envBrandingDir()).filePath(kManifestName)), manifestBefore);
}

void BrandIconsTest::firstRailRowUsesTransparentBrandLogo()
{
    QVERIFY(writeSyntheticLogo(logoPath(), kRedBlock));
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"logo\":\"mark.png\"}")));
    Branding::load();
    AppStyle::apply(*qobject_cast<QApplication*>(qApp));
    QString error;
    QVERIFY2(Database::instance().openAt(m_dataDir->path() + QStringLiteral("/brand.db"), &error), qPrintable(error));
    TaskStore store(Database::instance().connection());
    QVERIFY2(store.load(&error), qPrintable(error));
    TaskPanel panel(&store);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    auto* rail = panel.findChild<QFrame*>(QStringLiteral("ActivityRail"));
    auto* account = panel.findChild<QToolButton*>(QStringLiteral("AccountButton"));
    QVERIFY(rail && account && rail->layout());
        QCOMPARE(rail->layout()->itemAt(0)->widget(), static_cast<QWidget*>(account));
        // 顶部品牌 Logo 为 34px（Logo34），其余列首行中心线为 48。
        QCOMPARE(int(WorkspaceGeometry::RailAccountSize), 34);
        QCOMPARE(int(WorkspaceGeometry::FirstRowIconCenter), 48);
        QCOMPARE(account->size(), QSize(34, 34));
        QCOMPARE(account->iconSize(), QSize(34, 34));
        QCOMPARE(account->icon().cacheKey(), brandLogoIcon().cacheKey());
        QVERIFY(account->icon().cacheKey() != applicationBrandIcon().cacheKey());
        const QImage logo = account->icon().pixmap(QSize(34, 34)).toImage();
        QVERIFY(logo.pixelColor(0, 0).alpha() == 0);
        QCOMPARE(iconImage(applicationBrandIcon(), QSize(34, 34)).pixel(0, 0), ds::raw::White);
    QCOMPARE(account->accessibleName(), QStringLiteral("本机工作区"));
    const int logoCenter = account->mapTo(&panel, QPoint()).y() + account->height() / 2;
    const int sharedCenter = rail->mapTo(&panel, QPoint()).y()
        + WorkspaceGeometry::FirstRowIconCenter;
    QCOMPARE(logoCenter, sharedCenter);
}

int main(int argc, char *argv[])
{
    // CTest 已通过环境变量设置 offscreen；手动运行时兜底（须在 QApplication 之前）。
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    // 与 src/main.cpp 一致，覆盖高 DPR 图标路径。
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    BrandIconsTest tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_brandicons.moc"
