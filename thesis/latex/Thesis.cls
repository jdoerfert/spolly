%%
%% This is file `Thesis.cls', based on 'ECSthesis.cls', by Steve R. Gunn
%% generated with the docstrip utility.
%%
%% Created by Steve R. Gunn, modified by Sunil Patel: www.sunilpatel.co.uk

\NeedsTeXFormat{LaTeX2e}[1996/12/01]
\ProvidesClass{Thesis}
              [2007/22/02 v1.0
   LaTeX document class]
\def\baseclass{book}
\DeclareOption*{\PassOptionsToClass{\CurrentOption}{\baseclass}}
\def\@checkoptions#1#2{
  \edef\@curroptions{\@ptionlist{\@currname.\@currext}}
  \@tempswafalse
  \@tfor\@this:=#2\do{
    \@expandtwoargs\in@{,\@this,}{,\@curroptions,}
    \ifin@ \@tempswatrue \@break@tfor \fi}
  \let\@this\@empty
  \if@tempswa \else \PassOptionsToClass{#1}{\baseclass}\fi
}
\@checkoptions{11pt}{{10pt}{11pt}{12pt}}
\PassOptionsToClass{a4paper}{\baseclass}
\ProcessOptions\relax
\LoadClass{\baseclass}
\newcommand\bhrule{\typeout{------------------------------------------------------------------------------}}

\newcommand\Declaration[1]{
\btypeout{Declaration of Authorship}
\addtotoc{Declaration of Authorship}
\thispagestyle{plain}
\null\vfil
%\vskip 60\p@
\begin{center}{\huge\bf Declaration of Authorship\par}\end{center}
%\vskip 60\p@
{\normalsize #1}
\vfil\vfil\null
%\cleardoublepage
}

\newcommand\btypeout[1]{\bhrule\typeout{\space #1}\bhrule}
\def\today{\ifcase\month\or
  January\or February\or March\or April\or May\or June\or
  July\or August\or September\or October\or November\or December\fi
  \space \number\year}
\usepackage{setspace}
\onehalfspacing
\setlength{\parindent}{0pt}
\setlength{\parskip}{2.0ex plus0.5ex minus0.2ex}
\usepackage{vmargin}
\setmarginsrb           { 1.5in}  % left margin
                        { 0.6in}  % top margin
                        { 1.0in}  % right margin
                        { 0.8in}  % bottom margin
                        {  20pt}  % head height
                        {0.25in}  % head sep
                        {   9pt}  % foot height
                        { 0.3in}  % foot sep
\raggedbottom
\setlength{\topskip}{1\topskip \@plus 5\p@}
\doublehyphendemerits=10000       % No consecutive line hyphens.
\brokenpenalty=10000              % No broken words across columns/pages.
\widowpenalty=9999                % Almost no widows at bottom of page.
\clubpenalty=9999                 % Almost no orphans at top of page.
\interfootnotelinepenalty=9999    % Almost never break footnotes.
\usepackage{fancyhdr}
\lhead[\rm\thepage]{\fancyplain{}{\sl{\rightmark}}}
\rhead[\fancyplain{}{\sl{\leftmark}}]{\rm\thepage}
\chead{}\lfoot{}\rfoot{}\cfoot{}
\pagestyle{fancy}
\renewcommand{\chaptermark}[1]{\btypeout{\thechapter\space #1}\markboth{\@chapapp\ \thechapter\ #1}{\@chapapp\ \thechapter\ #1}}
\renewcommand{\sectionmark}[1]{}
\renewcommand{\subsectionmark}[1]{}
\def\cleardoublepage{\clearpage\if@twoside \ifodd\c@page\else
\hbox{}
\thispagestyle{empty}
\newpage
\if@twocolumn\hbox{}\newpage\fi\fi\fi}
\usepackage{amsmath,amsfonts,amssymb,amscd,amsthm,xspace}
\theoremstyle{plain}
\newtheorem{example}{Example}[chapter]
\newtheorem{theorem}{Theorem}[chapter]
\newtheorem{corollary}[theorem]{Corollary}
\newtheorem{lemma}[theorem]{Lemma}
\newtheorem{proposition}[theorem]{Proposition}
\newtheorem{axiom}[theorem]{Axiom}
\theoremstyle{definition}
\newtheorem{definition}[theorem]{Definition}
\theoremstyle{remark}
\newtheorem{remark}[theorem]{Remark}
\usepackage[centerlast,small,sc]{caption}
\setlength{\captionmargin}{20pt}
\newcommand{\fref}[1]{Figure~\ref{#1}}
\newcommand{\tref}[1]{Table~\ref{#1}}
\newcommand{\eref}[1]{Equation~\ref{#1}}
\newcommand{\cref}[1]{Chapter~\ref{#1}}
\newcommand{\sref}[1]{Section~\ref{#1}}
\newcommand{\aref}[1]{Appendix~\ref{#1}}
\renewcommand{\topfraction}{0.85}
\renewcommand{\bottomfraction}{.85}
\renewcommand{\textfraction}{0.1}
\renewcommand{\dbltopfraction}{.85}
\renewcommand{\floatpagefraction}{0.75}
\renewcommand{\dblfloatpagefraction}{.75}
\setcounter{topnumber}{9}
\setcounter{bottomnumber}{9}
\setcounter{totalnumber}{20}
\setcounter{dbltopnumber}{9}
\usepackage{graphicx}
\usepackage{epstopdf}
\usepackage{subfig}
\usepackage{booktabs}
\usepackage{rotating}
\usepackage{listings}
\usepackage{lstpatch}
\usepackage{color}
\usepackage[usenames,dvipsnames]{xcolor}
%\usepackage{titlesec}
\usepackage[subfigure]{tocloft}

\newcommand{\green}{
\definecolor{shadecolor}{named}{Green}
\colorlet{shadecolor}{Green!20}
}
\newcommand{\yellow}{
\definecolor{shadecolor}{named}{Yellow}
\colorlet{shadecolor}{Yellow!30}
}
\newcommand{\orange}{
\definecolor{shadecolor}{named}{Orange}
\colorlet{shadecolor}{Orange!40}
}
\newcommand{\red}{
\definecolor{shadecolor}{named}{Red}
\colorlet{shadecolor}{Red!60}
}

\renewcommand{\@makechapterhead}[1]{%
  \vspace*{60\p@}%
  %\vspace\fill%
  {\parindent \z@ \raggedright \normalfont
    \hrule                                           % horizontal line
    \vspace{5pt}%                                    % add some vertical space
    \ifnum \c@secnumdepth >\m@ne
        \huge\scshape \@chapapp\space \thechapter    % Chapter followed by number
        \par\nobreak
    \fi
    \interlinepenalty\@M
    \color{Maroon} \Huge \scshape #1\par             % chapter title
    \vspace{5pt}%                                    % add some vertical space
    \color{black}
    \hrule                                           % horizontal rule
    \nobreak
    \vskip 40\p@
  }}

% Unnumbered chapter heading style:

\renewcommand{\@makeschapterhead}[1]{%
  %\vspace*{60\p@}%
  %\vspace*{60\p@}%
  {\parindent \z@ \raggedright
    \normalfont
    \hrule                                           % horizontal line
    \vspace{5pt}%                                    % add some vertical space
    \interlinepenalty\@M
    \Huge \scshape #1\par                            % chapter title
    \vspace{5pt}%                                    % add some vertical space
    \hrule                                           % horizontal line
    \nobreak
    \vskip 40\p@
  }}

\newcommand{\chapterhead}[1]{%
  %\vspace*{60\p@}%
  %\vspace*{60\p@}%
  {\parindent \z@   
    \normalfont
    \hrule                                           % horizontal line
    \vspace{5pt}%                                    % add some vertical space
    \interlinepenalty\@M
    \Huge \scshape #1\par                            % chapter title
    \vspace{5pt}%                                    % add some vertical space
    \hrule                                           % horizontal line
    \nobreak
    \vskip 10\p@
  }}



\renewcommand*\cftchapfont{\hypersetup{linkcolor=Maroon} \bf}
\renewcommand{\cftfigfont}{Figure }
\renewcommand{\cfttabfont}{Table }

\renewcommand*\contentsname{\chapterhead{Content}}
\renewcommand*\listfigurename{\chapterhead{List of Figures}}
\renewcommand*\listtablename{\chapterhead{List of Tables}}

%\renewcommand{\cftsecindent}{0.4 em}
%\renewcommand{\cftsecnumwidth}{3.2 em}
%\renewcommand{\cftsubsecindent}{0.4 em}
%\renewcommand{\cftsubsecnumwidth}{3.2 em}
%\renewcommand{\cftsubsubsecindent}{0.4 em}
%\renewcommand{\cftsubsubsecnumwidth}{3.6 em}
%\renewcommand{\cftparaindent}{0.4 em}
%\renewcommand{\cftparanumwidth}{3.8 em}


%\setlength\cftparskip{-2pt}
%\setlength\cftbeforechapskip{0pt}

\newcommand{\resetlst}{\lstset{captionpos=b,
        frame=tb,
        basicstyle=\scriptsize\ttfamily,
        showstringspaces=false,
        keepspaces=true}}%
\resetlst%

\usepackage[pdfpagemode={UseOutlines},bookmarks=true,
   bookmarksopenlevel=0,bookmarksnumbered=true,hypertexnames=false,
   colorlinks,linkcolor={black},citecolor={Maroon},urlcolor={red},
   pdfstartview={FitV},unicode,breaklinks=true,bookmarksopen=true]{hyperref}
\usepackage{minitoc}
\mtcsetdepth{minitoc}{3}


\pdfstringdefDisableCommands{
   \let\\\space
}
\newcommand*{\supervisor}[1]{\def\supname{#1}}
\newcommand*{\examiner}[1]{\def\examname{#1}}
\newcommand*{\degree}[1]{\def\degreename{#1}}
\newcommand*{\authors}[1]{\def\authornames{#1}}
\newcommand*{\addresses}[1]{\def\addressnames{#1}}
\newcommand*{\university}[1]{\def\univname{#1}}
\newcommand*{\UNIVERSITY}[1]{\def\UNIVNAME{#1}}
\newcommand*{\department}[1]{\def\deptname{#1}}
\newcommand*{\DEPARTMENT}[1]{\def\DEPTNAME{#1}}
\newcommand*{\group}[1]{\def\groupname{#1}}
\newcommand*{\GROUP}[1]{\def\GROUPNAME{#1}}
\newcommand*{\faculty}[1]{\def\facname{#1}}
\newcommand*{\FACULTY}[1]{\def\FACNAME{#1}}
\newcommand*{\subject}[1]{\def\subjectname{#1}}
\newcommand*{\keywords}[1]{\def\keywordnames{#1}}
\supervisor  {Clemens Hammacher und Kevin Streit}
\examiner    {}
\degree      {Bachelor}
\authors     {Johannes Doerfert}
\university  {\texorpdfstring{\href{http://www.uni-saarland.de/}
                {Saarland University}}
                {Saarland University}}
\UNIVERSITY  {\texorpdfstring{\href{http://www.uni-saarland.de/}
                {SAARLAND UNIVERSITY}}
                {SAARLAND UNIVERSITY}}
\department  {\texorpdfstring{\href{http://www.cs.uni-saarland.de/}
                {Department of Computer Science}}
                {Department of Computer Science}}
\DEPARTMENT  {\texorpdfstring{\href{http://www.cs.uni-saarland.de/}
                {DEPARTMENT OF COMPUTER SCIENCE}}
                {DEPARTMENT OF COMPUTER SCIENCE}}
\group       {\texorpdfstring{\href{http://www.cdl.uni-saarland.de/}
                {Compiler Design Lab}}
                {Compiler Design Lab}}
\GROUP       {\texorpdfstring{\href{http://www.cdl.uni-saarland.de/}
                {COMPILER DESIGN LAB}}
                {COMPILER DESIGN LAB}}
\faculty     {\texorpdfstring{\href{http://www.uni-saarland.de/en/campus/faculties/faculties/faculty-6-natural-sciences-and-technology-i.html}
                {Faculty of Natural Sciences and Technology I}}
                {Faculty of Natural Sciences and Technology I}}
\FACULTY     {\texorpdfstring{\href{http://www.uni-saarland.de/en/campus/faculties/faculties/faculty-6-natural-sciences-and-technology-i.html}
                {FACULTY OF NATURAL SCIENCES AND TECHNOLOGY I}}
                {FACULTY OF NATURAL SCIENCES AND TECHNOLOGY I}}

\definecolor{Maroon}{HTML}{990000}

\addresses   {}
\subject     {}
\keywords    {}
\renewcommand\maketitle{
  \btypeout{Title Page}
  \hypersetup{pdftitle={\@title}}
  \hypersetup{pdfsubject=\subjectname}
  \hypersetup{pdfauthor=\authornames}
  \hypersetup{pdfkeywords=\keywordnames}
  \thispagestyle{empty}
  \begin{titlepage}
    \let\footnotesize\small
    \let\footnoterule\relax
    \let \footnote \thanks
    \setcounter{footnote}{0}
    \null\vfil
    \vskip 60\p@
    \begin{center}
      \setlength{\parskip}{0pt}
      {\LARGE \color{Maroon} \bf \TITLE \par}
      \smallskip
      {\authornames \par}
      \bigskip
      \bigskip
      \bigskip
      \bigskip
      \bigskip
      \bigskip
      {\Large Bachelor Thesis \par}
      \bigskip
      \bigskip
      \bigskip
      \bigskip
      {\large \groupname \par}
      {\large \facname \par}
      {\large \deptname \par}
      {\large \univname \par}
      \bigskip
      \bigskip
      \bigskip
      {\large Supervisor: Prof. Dr. Sebastian Hack \par}
      \smallskip
      {\large Advisors: Clemens Hammacher\par}
      {\large \hspace*{1.8mm} Kevin Streit\par} 
      \smallskip
      {\large Reviewers: Prof. Dr. Sebastian Hack \par}
      {\large \hspace*{18.45mm} Prof. Dr. Andreas Zeller \par}
      \smallskip
      \bigskip
      {\large Submitted: \@date \par}
      \bigskip
      \bigskip
      \bigskip
      \bigskip
      {\includegraphics[height=2cm]{UniSBLogo.pdf}}
    \end{center}
    \par
    \@thanks
    \vfil\null
  \end{titlepage}
  \setcounter{footnote}{0}%
  \global\let\thanks\relax
  \global\let\maketitle\relax
  \global\let\@thanks\@empty
  \global\let\@author\@empty
  \global\let\@date\@empty
  \global\let\@title\@empty
  \global\let\title\relax
  \global\let\author\relax
  \global\let\date\relax
  \global\let\and\relax
  \cleardoublepage
}
\newenvironment{abstract}
{
  \btypeout{Abstract Page}
  \thispagestyle{empty}
  \null\vfil
  \begin{center}
    \setlength{\parskip}{0pt}
    {\normalsize \UNIVNAME \par}
    \bigskip
    {\huge{\textit{\Abstract}} \par}
    \bigskip
    {\normalsize \groupname \par}
    {\normalsize \deptname \par}
    \bigskip
    {\normalsize Bachelor of Science \par}
    \bigskip
    {\normalsize\bf \@title \par}
    \medskip
    {\normalsize by \authornames \par}
    \bigskip
  \end{center}
}
{
  \vfil\vfil\vfil\null
  \cleardoublepage
}
\addtocounter{secnumdepth}{1}
\setcounter{tocdepth}{2}
\newcounter{dummy}
\newcommand\addtotoc[1]{
\refstepcounter{dummy}
\addcontentsline{toc}{chapter}{#1}}
\renewcommand\tableofcontents{
\btypeout{Table of Contents}
\begin{spacing}{1}{
    \setlength{\parskip}{1pt}
    \if@twocolumn
      \@restonecoltrue\onecolumn
    \else
      \@restonecolfalse
    \fi
    \chapter*{\contentsname
        \@mkboth{
           \MakeUppercase\contentsname}{\MakeUppercase\contentsname}}
    \@starttoc{toc}
    \if@restonecol\twocolumn\fi
   \cleardoublepage
}\end{spacing}
}
\renewcommand\listoffigures{
\btypeout{List of Figures}
\addtotoc{List of Figures}
\begin{spacing}{1}{
    \setlength{\parskip}{1pt}
    \if@twocolumn
      \@restonecoltrue\onecolumn
    \else
      \@restonecolfalse
    \fi
    \chapter*{\listfigurename
      \@mkboth{\MakeUppercase\listfigurename}
              {\MakeUppercase\listfigurename}}
    \@starttoc{lof}
    \if@restonecol\twocolumn\fi
    \cleardoublepage
}\end{spacing}
}
\renewcommand\listoftables{
\btypeout{List of Tables}
\addtotoc{List of Tables}
\begin{spacing}{1}{
    \setlength{\parskip}{1pt}
    \if@twocolumn
      \@restonecoltrue\onecolumn
    \else
      \@restonecolfalse
    \fi
    \chapter*{\listtablename
      \@mkboth{
          \MakeUppercase\listtablename}{\MakeUppercase\listtablename}}
    \@starttoc{lot}
    \if@restonecol\twocolumn\fi
    \cleardoublepage
}\end{spacing}
}
\newcommand\listsymbolname{Abbreviations}
\usepackage{longtable}
\newcommand\listofsymbols[2]{
\btypeout{\listsymbolname}
\addtotoc{\listsymbolname} 
    \chapter*{\listsymbolname
      \@mkboth{
          \MakeUppercase\listsymbolname}{\MakeUppercase\listsymbolname}}
\begin{longtable}[c]{#1}#2\end{longtable}\par
    \cleardoublepage
}
\newcommand\listconstants{Physical Constants}
\usepackage{longtable}
\newcommand\listofconstants[2]{
\btypeout{\listconstants}
%\addtotoc{\listconstants}
    \chapter*{\listconstants
      \@mkboth{
          \MakeUppercase\listconstants}{\MakeUppercase\listconstants}}
\begin{longtable}[c]{#1}#2\end{longtable}\par
    \cleardoublepage
}
\newcommand\listnomenclature{Symbols}
\usepackage{longtable}
\newcommand\listofnomenclature[2]{
\btypeout{\listnomenclature}
%\addtotoc{\listnomenclature}
    \chapter*{\listnomenclature
      \@mkboth{
          \MakeUppercase\listnomenclature}{\MakeUppercase\listnomenclature}}
\begin{longtable}[c]{#1}#2\end{longtable}\par
    \cleardoublepage
}
\newcommand\acknowledgements[1]{
\btypeout{Acknowledgements}
\addtotoc{Acknowledgements}
\thispagestyle{empty}
\begin{center}{\huge{\textit{Acknowledgements}} \par}\end{center}
{\normalsize #1}
\vfil\vfil\null

}
\newcommand\dedicatory[1]{
\btypeout{Dedicatory}
\thispagestyle{plain}
\null\vfil
\vskip 60\p@
\begin{center}{\Large \sl #1}\end{center}
\vfil\null
\cleardoublepage
}
\renewcommand\backmatter{
  \if@openright
    \cleardoublepage
  \else
    \clearpage
  \fi
  \addtotoc{\bibname}
  \btypeout{\bibname}
  \@mainmatterfalse}
\endinput
%%
%% End of file `Thesis.cls'.
